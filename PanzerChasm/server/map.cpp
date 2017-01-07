#include <cstring>

#include <matrix.hpp>

#include "../game_constants.hpp"
#include "../math_utils.hpp"
#include "../particles.hpp"
#include "../sound/sound_id.hpp"
#include "a_code.hpp"
#include "collisions.hpp"
#include "collision_index.inl"
#include "monster.hpp"
#include "player.hpp"

#include "map.hpp"

namespace PanzerChasm
{

static const float g_commands_coords_scale= 1.0f / 256.0f;

template<class Wall>
static m_Vec3 GetNormalForWall( const Wall& wall )
{
	m_Vec3 n( wall.vert_pos[0].y - wall.vert_pos[1].y, wall.vert_pos[1].x - wall.vert_pos[0].x, 0.0f );
	return n / n.xy().Length();
}

Map::Rocket::Rocket(
	const EntityId in_rocket_id,
	const EntityId in_owner_id,
	const unsigned char in_rocket_type_id,
	const m_Vec3& in_start_point,
	const m_Vec3& in_normalized_direction,
	const Time in_start_time )
	: start_time( in_start_time )
	, start_point( in_start_point )
	, normalized_direction( in_normalized_direction )
	, rocket_id( in_rocket_id )
	, owner_id( in_owner_id )
	, rocket_type_id( in_rocket_type_id )
	, previous_position( in_start_point )
	, track_length( 0.0f )
{}

bool Map::Rocket::HasInfiniteSpeed( const GameResources& game_resources ) const
{
	PC_ASSERT( rocket_type_id < game_resources.rockets_description.size() );
	return game_resources.rockets_description[ rocket_type_id ].model_file_name[0] == '\0';
}

template<class Func>
void Map::ProcessElementLinks(
	const MapData::IndexElement::Type element_type,
	const unsigned int index,
	const Func& func )
{
	for( const MapData::Link& link : map_data_->links )
	{
		const MapData::IndexElement& index_element= map_data_->map_index[ link.x + link.y * MapData::c_map_size ];

		if( index_element.type == element_type && index_element.index == index )
			func( link );
	}
}

Map::Map(
	const DifficultyType difficulty,
	const MapDataConstPtr& map_data,
	const GameResourcesConstPtr& game_resources,
	const Time map_start_time,
	MapEndCallback map_end_callback )
	: difficulty_(difficulty)
	, map_data_(map_data)
	, game_resources_(game_resources)
	, map_end_callback_( std::move( map_end_callback ) )
	, random_generator_( std::make_shared<LongRand>() )
	, collision_index_( map_data )
{
	PC_ASSERT( map_data_ != nullptr );
	PC_ASSERT( game_resources_ != nullptr );

	std::memset( wind_field_, 0, sizeof(wind_field_) );
	std::memset( death_field_, 0, sizeof(death_field_) );

	procedures_.resize( map_data_->procedures.size() );
	for( unsigned int p= 0u; p < procedures_.size(); p++ )
	{
		if( map_data_->procedures[p].locked )
			procedures_[p].locked= true;
	}

	dynamic_walls_.resize( map_data_->dynamic_walls.size() );
	for( unsigned int w= 0u; w < dynamic_walls_.size(); w++ )
	{
		dynamic_walls_[w].texture_id= map_data_->dynamic_walls[w].texture_id;
	}

	static_models_.resize( map_data_->static_models.size() );
	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		const MapData::StaticModel& in_model= map_data_->static_models[m];
		StaticModel& out_model= static_models_[m];

		out_model.model_id= in_model.model_id;

		const MapData::ModelDescription* const model_description=
			in_model.model_id < map_data_->models_description.size()
				? &map_data_->models_description[ in_model.model_id ] : nullptr;

		out_model.health= model_description == nullptr ? 0 : model_description->break_limit;

		out_model.pos= m_Vec3( in_model.pos, 0.0f );
		out_model.angle= in_model.angle;
		out_model.baze_z= 0.0f;

		if( model_description != nullptr &&
			static_cast<ACode>(model_description->ac) == ACode::Switch )
			out_model.animation_state= StaticModel::AnimationState::SingleFrame;
		else
			out_model.animation_state= StaticModel::AnimationState::Animation;

		out_model.animation_start_time= map_start_time;
		out_model.animation_start_frame= 0u;
	}

	items_.resize( map_data_->items.size() );
	for( unsigned int i= 0u; i < items_.size(); i++ )
	{
		const MapData::Item& in_item= map_data_->items[i];
		Item& out_item= items_[i];

		out_item.item_id= in_item.item_id;
		out_item.pos= m_Vec3( in_item.pos, 0.0f );
		out_item.picked_up= false;
	}

	// Pull up items, which placed atop of models
	for( Item& item : items_ )
	{
		item.pos.z= GetFloorLevel( item.pos.xy(), GameConstants::player_interact_radius );
	}
	for( StaticModel& model : static_models_ )
	{
		if( model.model_id >= map_data_->models_description.size() )
			continue;

		const MapData::ModelDescription& description= map_data_->models_description[ model.model_id ];
		if( description.ac == 0u )
			continue;

		// HACK for keys. Use nonzero radius.
		const float radius= std::max( description.radius, GameConstants::player_interact_radius);
		model.pos.z= model.baze_z= GetFloorLevel( model.pos.xy(), radius );
	}

	// Spawn monsters
	for( const MapData::Monster& map_monster : map_data_->monsters )
	{
		// Skip players
		if( map_monster.monster_id == 0u )
			continue;

		if( ( map_monster.difficulty_flags & difficulty_ ) == 0u )
			continue;

		monsters_[ GetNextMonsterId() ]=
			MonsterPtr(
				new Monster(
					map_monster,
					GetFloorLevel( map_monster.pos ),
					game_resources_,
					random_generator_,
					map_start_time ) );
	}
}

Map::~Map()
{}

DifficultyType Map::GetDifficulty() const
{
	return difficulty_;
}

EntityId Map::SpawnPlayer( const PlayerPtr& player )
{
	PC_ASSERT( player != nullptr );

	unsigned int min_spawn_number= ~0u;
	const MapData::Monster* spawn_with_min_number= nullptr;

	for( const MapData::Monster& monster : map_data_->monsters )
	{
		if( monster.difficulty_flags < min_spawn_number )
		{
			min_spawn_number= monster.difficulty_flags;
			spawn_with_min_number= &monster;
		}
	}

	if( spawn_with_min_number != nullptr )
	{
		player->Teleport(
			m_Vec3(
				spawn_with_min_number->pos,
				GetFloorLevel( spawn_with_min_number->pos, GameConstants::player_radius ) ),
			spawn_with_min_number->angle );
	}
	else
		player->SetPosition( m_Vec3( 0.0f, 0.0f, 4.0f ) );

	player->SetRandomGenerator( random_generator_ );
	player->ResetActivatedProcedure();

	const EntityId player_id= GetNextMonsterId();

	players_.emplace( player_id, player );
	monsters_.emplace( player_id, player );

	return player_id;
}

void Map::Shoot(
	const EntityId owner_id,
	const unsigned int rocket_id,
	const m_Vec3& from,
	const m_Vec3& normalized_direction,
	const Time current_time )
{
	rockets_.emplace_back( next_rocket_id_, owner_id, rocket_id, from, normalized_direction, current_time );
	next_rocket_id_++;

	Rocket& rocket= rockets_.back();
	if( !rocket.HasInfiniteSpeed( *game_resources_ ) )
	{
		Messages::RocketBirth message;

		message.rocket_id= rocket.rocket_id;
		message.rocket_type= rocket.rocket_type_id;

		PositionToMessagePosition( rocket.start_point, message.xyz );

		float angle[2];
		VecToAngles( rocket.normalized_direction, angle );
		for( unsigned int j= 0u; j < 2u; j++ )
			message.angle[j]= AngleToMessageAngle( angle[j] );

		rockets_birth_messages_.emplace_back( message );
	}

	// Set initial speed for jumping rockets.
	const GameResources::RocketDescription& description= game_resources_->rockets_description[ rocket.rocket_type_id ];
	if( description.reflect )
	{
		const float speed= description.fast ? GameConstants::fast_rockets_speed : GameConstants::rockets_speed;
		rocket.speed= rocket.normalized_direction * speed;
	}
}

void Map::PlantMine( const m_Vec3& pos, const Time current_time )
{
	mines_.emplace_back();
	Mine& mine= mines_.back();
	mine.pos= pos;
	mine.pos.z= GetFloorLevel( pos.xy(), 0.2f/* TODO - select correct radius*/ );
	mine.planting_time= current_time;
	mine.id= next_rocket_id_;
	next_rocket_id_++;

	dynamic_items_birth_messages_.emplace_back();
	Messages::DynamicItemBirth& message= dynamic_items_birth_messages_.back();
	message.item_id= mine.id;
	message.item_type_id= 30u; // id of mine item
	PositionToMessagePosition( mine.pos, message.xyz );

	PlayMapEventSound( mine.pos, Sound::SoundId::MineOn );
}

void Map::SpawnMonsterBodyPart(
	const unsigned char monster_type_id, const unsigned char body_part_id,
	const m_Vec3& pos, float angle )
{
	monsters_parts_birth_messages_.emplace_back();
	Messages::MonsterPartBirth& message= monsters_parts_birth_messages_.back();

	message.monster_type= monster_type_id;
	message.part_id= body_part_id;

	PositionToMessagePosition( pos, message.xyz );
	message.angle= AngleToMessageAngle( angle );
}

void Map::PlayMonsterLinkedSound(
	const EntityId monster_id,
	const unsigned int sound_id )
{
	monster_linked_sounds_messages_.emplace_back();
	Messages::MonsterLinkedSound& message= monster_linked_sounds_messages_.back();

	message.monster_id= monster_id;
	message.sound_id= sound_id;
}

void Map::PlayMonsterSound(
	const EntityId monster_id,
	const unsigned int monster_sound_id )
{
	monsters_sounds_messages_.emplace_back();
	Messages::MonsterSound& message= monsters_sounds_messages_.back();

	message.monster_id= monster_id;
	message.monster_sound_id= monster_sound_id;
}

void Map::PlayMapEventSound( const m_Vec3& pos, const unsigned int sound_id )
{
	map_events_sounds_messages_.emplace_back();
	Messages::MapEventSound& message= map_events_sounds_messages_.back();

	PositionToMessagePosition( pos, message.xyz );
	message.sound_id= sound_id;
}

m_Vec3 Map::CollideWithMap( const m_Vec3 in_pos, const float height, const float radius, bool& out_on_floor ) const
{
	m_Vec2 pos= in_pos.xy();
	out_on_floor= false;

	const float z_bottom= in_pos.z;
	const float z_top= z_bottom + height;
	float new_z= in_pos.z;

	const auto elements_process_func=
	[&]( const MapData::IndexElement& index_element )
	{
		if( index_element.type == MapData::IndexElement::StaticWall )
		{
			PC_ASSERT( index_element.index < map_data_->static_walls.size() );
			const MapData::Wall& wall= map_data_->static_walls[ index_element.index ];

			const MapData::WallTextureDescription& tex= map_data_->walls_textures[ wall.texture_id ];
			if( tex.gso[0] )
				goto end;

			m_Vec2 new_pos;
			if( CollideCircleWithLineSegment(
					wall.vert_pos[0], wall.vert_pos[1],
					pos, radius,
					new_pos ) )
			{
				pos= new_pos;
			}
		}
		else if( index_element.type == MapData::IndexElement::StaticModel )
		{
			const StaticModel& model= static_models_[ index_element.index ];
			if( model.model_id >= map_data_->models_description.size() )
				goto end;

			const MapData::ModelDescription& model_description= map_data_->models_description[ model.model_id ];
			if( model_description.radius <= 0.0f )
				goto end;

			const Model& model_geometry= map_data_->models[ model.model_id ];

			const float model_z_min= model_geometry.z_min + model.pos.z;
			const float model_z_max= model_geometry.z_max + model.pos.z;
			if( z_top < model_z_min || z_bottom > model_z_max )
				goto end;

			const float min_distance= radius + model_description.radius;

			const m_Vec2 vec_to_pos= pos - model.pos.xy();
			const float square_distance= vec_to_pos.SquareLength();

			if( square_distance <= min_distance * min_distance )
			{
				// Pull up or down player.
				if( model_geometry.z_max - z_bottom <= GameConstants::player_z_pull_distance )
				{
					new_z= std::max( new_z, model_z_max );
					out_on_floor= true;
				}
				else if( z_top - model_geometry.z_min <= GameConstants::player_z_pull_distance )
					new_z= std::min( new_z, model_z_min - height );
				// Push sideways.
				else
					pos= model.pos.xy() + vec_to_pos * ( min_distance / std::sqrt( square_distance ) );
			}
		}
		else
		{
			// TODO
		}
		end:;
	};

	collision_index_.ProcessElementsInRadius(
		pos, radius,
		elements_process_func );

	// Dynamic walls
	for( const DynamicWall& wall : dynamic_walls_ )
	{
		if( wall.vert_pos[0] == wall.vert_pos[1] )
			continue;

		const MapData::WallTextureDescription& tex= map_data_->walls_textures[ wall.texture_id ];
		if( tex.gso[0] )
			continue;

		if( z_top < wall.z || z_bottom > wall.z + GameConstants::walls_height )
			continue;

		m_Vec2 new_pos;
		if( CollideCircleWithLineSegment(
				wall.vert_pos[0], wall.vert_pos[1],
				pos, radius,
				new_pos ) )
		{
			pos= new_pos;
		}
	}

	if( new_z <= 0.0f )
	{
		out_on_floor= true;
		new_z= 0.0f;
	}
	else if( new_z + height > GameConstants::walls_height )
		new_z= GameConstants::walls_height - height;

	return m_Vec3( pos, new_z );
}

bool Map::CanSee( const m_Vec3& from, const m_Vec3& to ) const
{
	if( from == to )
		return true;

	m_Vec3 direction= to - from;
	const float max_see_distance= direction.Length();
	direction.Normalize();

	bool can_see= true;
	const auto try_set_occluder=
	[&]( const m_Vec3& intersection_point ) -> bool
	{
		if( ( intersection_point - from ).SquareLength() <= max_see_distance * max_see_distance )
		{
			can_see= false;
			return true;
		}
		return false;
	};

	const auto element_process_func=
	[&]( const MapData::IndexElement& element ) -> bool
	{
		if( element.type == MapData::IndexElement::StaticWall )
		{
			PC_ASSERT( element.index < map_data_->static_walls.size() );
			const MapData::Wall& wall= map_data_->static_walls[ element.index ];

			const MapData::WallTextureDescription& wall_texture= map_data_->walls_textures[ wall.texture_id ];
			if( wall_texture.gso[1] )
				goto end;

			m_Vec3 candidate_pos;
			if( RayIntersectWall(
					wall.vert_pos[0], wall.vert_pos[1],
					0.0f, 2.0f,
					from, direction,
					candidate_pos ) )
			{
				if( try_set_occluder( candidate_pos ) )
					return true;
			}
		}
		else if( element.type == MapData::IndexElement::StaticModel )
		{
			PC_ASSERT( element.index < static_models_.size() );
			const StaticModel& model= static_models_[ element.index ];

			if( model.model_id >= map_data_->models_description.size() )
				goto end;

			const MapData::ModelDescription& model_description= map_data_->models_description[ model.model_id ];
			if( model_description.radius <= 0.0f )
				goto end;

			const Model& model_data= map_data_->models[ model.model_id ];

			m_Vec3 candidate_pos;
			if( RayIntersectCylinder(
					model.pos.xy(), model_description.radius,
					model_data.z_min + model.pos.z,
					model_data.z_max + model.pos.z,
					from, direction,
					candidate_pos ) )
			{
				if( try_set_occluder( candidate_pos ) )
					return true;
			}
		}
		else
		{
			PC_ASSERT( false );
		}

		end:
		return false;
	};

	// Static walls and map models.
	collision_index_.RayCast(
		from, direction,
		element_process_func,
		max_see_distance );

	// Dynamic walls.
	for( const DynamicWall& wall : dynamic_walls_ )
	{
		const MapData::WallTextureDescription& wall_texture= map_data_->walls_textures[ wall.texture_id ];
		if( wall_texture.gso[1] )
			continue;

		m_Vec3 candidate_pos;
		if( RayIntersectWall(
				wall.vert_pos[0], wall.vert_pos[1],
				wall.z, wall.z + 2.0f,
				from, direction,
				candidate_pos ) )
		{
			if( try_set_occluder( candidate_pos ) )
				break;
		}
	}

	return can_see;
}

const Map::PlayersContainer& Map::GetPlayers() const
{
	return players_;
}

void Map::ProcessPlayerPosition(
	const Time current_time,
	const EntityId player_monster_id,
	MessagesSender& messages_sender )
{
	const auto player_it= monsters_.find( player_monster_id );
	PC_ASSERT( player_it != monsters_.end() );
	Player& player= static_cast<Player&>( *(player_it->second) );

	const int player_x= static_cast<int>( std::floor( player.Position().x ) );
	const int player_y= static_cast<int>( std::floor( player.Position().y ) );
	if( player_x < 0 || player_y < 0 ||
		player_x >= int(MapData::c_map_size) ||
		player_y >= int(MapData::c_map_size) )
		return;

	// Process floors
	for( int x= std::max( 0, int(player_x) - 2); x < std::min( int(MapData::c_map_size), int(player_x) + 2 ); x++ )
	for( int y= std::max( 0, int(player_y) - 2); y < std::min( int(MapData::c_map_size), int(player_y) + 2 ); y++ )
	{
		// TODO - select correct player radius for floor collisions.
		if( !CircleIntersectsWithSquare(
				player.Position().xy(), GameConstants::player_radius, x, y ) )
			continue;

		for( const MapData::Link& link : map_data_->links )
		{
			if( link.type == MapData::Link::Floor && link.x == x && link.y == y )
				TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
			else if( link.type == MapData::Link::ReturnFloor && link.x == x && link.y == y )
				ReturnProcedure( link.proc_id, current_time );
		}
	}

	const m_Vec2 pos= player.Position().xy();
	const float z_bottom= player.Position().z;
	const float z_top= player.Position().z + GameConstants::player_height;

	// Static walls links.
	for( const MapData::Wall& wall : map_data_->static_walls )
	{
		if( wall.vert_pos[0] == wall.vert_pos[1] )
			continue;

		const MapData::WallTextureDescription& tex= map_data_->walls_textures[ wall.texture_id ];
		if( tex.gso[0] )
			continue;

		m_Vec2 new_pos;
		if( CollideCircleWithLineSegment(
				wall.vert_pos[0], wall.vert_pos[1],
				pos, GameConstants::player_interact_radius,
				new_pos ) )
		{
			ProcessElementLinks(
				MapData::IndexElement::StaticWall,
				&wall - map_data_->static_walls.data(),
				[&]( const MapData::Link& link )
				{
					if( link.type == MapData::Link::Link_ )
						TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
					else if( link.type == MapData::Link::Return )
						ReturnProcedure( link.proc_id, current_time );
				} );
		}
	}

	// Dynamic walls links.
	for( unsigned int w= 0u; w < dynamic_walls_.size(); w++ )
	{
		const DynamicWall& wall= dynamic_walls_[w];
		const MapData::Wall& map_wall= map_data_->dynamic_walls[w];

		if( wall.vert_pos[0] == wall.vert_pos[1] )
			continue;

		const MapData::WallTextureDescription& tex= map_data_->walls_textures[ map_wall.texture_id ];
		if( tex.gso[0] )
			continue;

		if( z_top < wall.z || z_bottom > wall.z + GameConstants::walls_height )
			continue;

		m_Vec2 new_pos;
		if( CollideCircleWithLineSegment(
				wall.vert_pos[0], wall.vert_pos[1],
				pos, GameConstants::player_interact_radius,
				new_pos ) )
		{
			ProcessElementLinks(
				MapData::IndexElement::DynamicWall,
				w,
				[&]( const MapData::Link& link )
				{
					if( link.type == MapData::Link::Link_ )
						TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
					else if( link.type == MapData::Link::Return )
						ReturnProcedure( link.proc_id, current_time );
				} );
		}
	}

	// Models links.
	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		const StaticModel& model= static_models_[m];
		if( model.model_id >= map_data_->models_description.size() )
			continue;

		const MapData::ModelDescription& model_description= map_data_->models_description[ model.model_id ];

		const Model& model_geometry= map_data_->models[ model.model_id ];

		const float model_z_min= model_geometry.z_min + model.pos.z;
		const float model_z_max= model_geometry.z_max + model.pos.z;
		if( z_top < model_z_min || z_bottom > model_z_max )
			continue;

		const float min_distance= GameConstants::player_interact_radius + model_description.radius;

		const m_Vec2 vec_to_player_pos= pos - model.pos.xy();
		const float square_distance= vec_to_player_pos.SquareLength();

		if( square_distance <= min_distance * min_distance )
		{
			// Links must work for zero radius
			ProcessElementLinks(
				MapData::IndexElement::StaticModel,
				m,
				[&]( const MapData::Link& link )
				{
					if( link.type == MapData::Link::Link_ )
						TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
					else if( link.type == MapData::Link::Return )
						ReturnProcedure( link.proc_id, current_time );
				} );
		}
	}

	// Process "special" models.
	// Pick-up keys.
	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		StaticModel& model= static_models_[m];
		const MapData::StaticModel& map_model= map_data_->static_models[m];

		if( map_model.model_id >= map_data_->models_description.size() )
			continue;

		const MapData::ModelDescription& model_description= map_data_->models_description[ map_model.model_id ];

		const ACode a_code= static_cast<ACode>( model_description.ac );
		if( ! model.picked && a_code >= ACode::RedKey && a_code <= ACode::BlueKey )
		{
			const m_Vec2 vec_to_player_pos= pos - model.pos.xy();
			const float square_distance= vec_to_player_pos.SquareLength();
			const float min_length= GameConstants::player_radius + model_description.radius;
			if( square_distance <= min_length * min_length )
			{
				model.picked= true;

				if( a_code == ACode::RedKey )
					player.GiveRedKey();
				if( a_code == ACode::GreenKey )
					player.GiveGreenKey();
				if( a_code == ACode::BlueKey )
					player.GiveBlueKey();

				PlayMonsterLinkedSound( player_monster_id, Sound::SoundId::GetKey );

				ProcessElementLinks(
					MapData::IndexElement::StaticModel,
					m,
					[&]( const MapData::Link& link )
					{
						if( link.type == MapData::Link::Link_ )
							TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
					} );
			}
		}
	}

	//Process items
	for( Item& item : items_ )
	{
		if( item.picked_up )
			continue;

		const float square_distance= ( item.pos.xy() - pos ).SquareLength();
		if( square_distance <= GameConstants::player_interact_radius * GameConstants::player_interact_radius )
		{
			item.picked_up= player.TryPickupItem( item.item_id );
			if( item.picked_up )
			{
				const ACode a_code= static_cast<ACode>( game_resources_->items_description[ item.item_id ].a_code );
				if( a_code >= ACode::Weapon_First && a_code <= ACode::Weapon_Last )
				{
					PlayMonsterLinkedSound(
						player_monster_id,
						Sound::SoundId::FirstWeaponPickup + static_cast<unsigned int>(a_code) - static_cast<unsigned int>(ACode::Weapon_First) );
				}
				if( a_code == ACode::Item_Life || a_code == ACode::Item_BigLife )
					PlayMonsterLinkedSound( player_monster_id, Sound::SoundId::Health );
				else if( a_code >= ACode::Ammo_First && a_code <= ACode::Ammo_Last )
					PlayMonsterLinkedSound( player_monster_id, Sound::SoundId::FirstWeaponPickup + 1u );
				else
					PlayMonsterLinkedSound( player_monster_id, Sound::SoundId::ItemUp );

				// Try activate item links
				ProcessElementLinks(
					MapData::IndexElement::Item,
					&item - items_.data(),
					[&]( const MapData::Link& link )
					{
						if( link.type == MapData::Link::Link_ )
							TryActivateProcedure( link.proc_id, current_time, player, messages_sender );
					} );
			}
		}
	}
}

void Map::Tick( const Time current_time, const Time last_tick_delta )
{
	const Time prev_tick_time= current_time - last_tick_delta;
	const unsigned int death_ticks=
		static_cast<unsigned int>( GameConstants::death_ticks_per_second * current_time  .ToSeconds() ) -
		static_cast<unsigned int>( GameConstants::death_ticks_per_second * prev_tick_time.ToSeconds() );

	const float last_tick_delta_s= last_tick_delta.ToSeconds();

	// Update state of procedures
	for( unsigned int p= 0u; p < procedures_.size(); p++ )
	{
		const MapData::Procedure& procedure= map_data_->procedures[p];
		ProcedureState& procedure_state= procedures_[p];

		const Time time_since_last_state_change= current_time - procedure_state.last_state_change_time;
		const float new_stage=
			procedure.speed > 0.0f
				? ( time_since_last_state_change.ToSeconds() * procedure.speed * GameConstants::procedures_speed_scale )
				: 1.0f;

		// Check map end
		if( procedure_state.movement_state != ProcedureState::MovementState::None &&
			procedure.end_delay_s > 0.0f &&
			time_since_last_state_change.ToSeconds() >= procedure.end_delay_s )
			map_end_triggered_= true;

		switch( procedure_state.movement_state )
		{
		case ProcedureState::MovementState::None:
			break;

		case ProcedureState::MovementState::StartWait:
			if( time_since_last_state_change.ToSeconds() >= procedure.start_delay_s )
			{
				ActivateProcedureSwitches( procedure, false, current_time );
				DoProcedureImmediateCommands( procedure );
				procedure_state.movement_state= ProcedureState::MovementState::Movement;
				procedure_state.movement_stage= 0.0f;
				procedure_state.last_state_change_time= current_time;
			}
			else
				procedure_state.movement_stage= new_stage;
			break;

		case ProcedureState::MovementState::Movement:
			if( new_stage >= 1.0f )
			{
				// TODO - do it at the end if movement?
				// Maybe, do this at end of reverse-movement?
				DoProcedureDeactivationCommands( procedure );

				procedure_state.movement_state= ProcedureState::MovementState::BackWait;
				procedure_state.movement_stage= 0.0f;
				procedure_state.last_state_change_time= current_time;
			}
			else
				procedure_state.movement_stage= new_stage;
			break;

		case ProcedureState::MovementState::BackWait:
		{
			const Time wait_time= current_time - procedure_state.last_state_change_time;
			if(
				procedure.back_wait_s > 0.0f &&
				wait_time.ToSeconds() >= procedure.back_wait_s )
			{
				ActivateProcedureSwitches( procedure, true, current_time );
				procedure_state.movement_state= ProcedureState::MovementState::ReverseMovement;
				procedure_state.movement_stage= 0.0f;
				procedure_state.last_state_change_time= current_time;
			}
		}
			break;

		case ProcedureState::MovementState::ReverseMovement:
			if( new_stage >= 1.0f )
			{
				procedure_state.movement_state= ProcedureState::MovementState::None;
				procedure_state.movement_stage= 0.0f;
				procedure_state.last_state_change_time= current_time;
			}
			else
				procedure_state.movement_stage= new_stage;
			break;
		}; // switch state
	} // for procedures

	MoveMapObjects();

	// Process static models
	for( StaticModel& model : static_models_ )
	{
		const float time_delta_s= ( current_time - model.animation_start_time ).ToSeconds();
		const float animation_frame= time_delta_s * GameConstants::animations_frames_per_second;

		if( model.animation_state == StaticModel::AnimationState::Animation )
		{
			if( model.model_id < map_data_->models.size() )
			{
				const Model& model_geometry= map_data_->models[ model.model_id ];

				model.current_animation_frame=
					static_cast<unsigned int>( std::round(animation_frame) ) % model_geometry.frame_count;
			}
			else
				model.current_animation_frame= 0u;
		}
		else if( model.animation_state == StaticModel::AnimationState::SingleAnimation )
		{
			if( model.model_id < map_data_->models.size() )
			{
				const Model& model_geometry= map_data_->models[ model.model_id ];

				const unsigned int animation_frame_integer= static_cast<unsigned int>( std::round(animation_frame) );
				if( animation_frame_integer >= model_geometry.frame_count - 1u )
				{
					model.animation_state= StaticModel::AnimationState::SingleFrame;
					model.animation_start_frame= model_geometry.frame_count - 1u;
				}
				else
					model.current_animation_frame= animation_frame_integer;
			}
			else
				model.current_animation_frame= 0u;
		}
		else if( model.animation_state == StaticModel::AnimationState::SingleReverseAnimation )
		{
			if( model.model_id < map_data_->models.size() )
			{
				const int animation_frame_integer=
					int(model.animation_start_frame) - static_cast<int>( std::round(animation_frame) );
				if( animation_frame_integer <= 0 )
				{
					model.animation_state= StaticModel::AnimationState::SingleFrame;
					model.animation_start_frame= 0u;
				}
				else
					model.current_animation_frame= animation_frame_integer;
			}
			else
				model.current_animation_frame= 0u;
		}
		else if( model.animation_state == StaticModel::AnimationState::SingleFrame )
			model.current_animation_frame= model.animation_start_frame;
		else
			model.current_animation_frame= model.animation_start_frame;
	} // for static models

	// Process shots
	for( unsigned int r= 0u; r < rockets_.size(); )
	{
		Rocket& rocket= rockets_[r];
		const GameResources::RocketDescription& rocket_description= game_resources_->rockets_description[ rocket.rocket_type_id ];

		const bool has_infinite_speed= rocket.HasInfiniteSpeed( *game_resources_ );
		const float time_delta_s= ( current_time - rocket.start_time ).ToSeconds();

		HitResult hit_result;

		if( has_infinite_speed )
			hit_result= ProcessShot( rocket.start_point, rocket.normalized_direction, Constants::max_float, rocket.owner_id );
		else
		{
			const float c_length_eps= 1.0f / 64.0f;
			const float gravity_force= GameConstants::rockets_gravity_scale * float( rocket_description.gravity_force );
			const float speed= rocket_description.fast ? GameConstants::fast_rockets_speed : GameConstants::rockets_speed;

			m_Vec3 new_pos;
			if( rocket_description.reflect )
			{
				rocket.speed.z-= gravity_force * last_tick_delta_s;
				new_pos= rocket.previous_position + rocket.speed * last_tick_delta_s;

				if( new_pos.z < 0.0f ) // Reflect.
				{
					new_pos.z= 0.0f;
					rocket.speed.z= std::abs( rocket.speed.z );
				}

				rocket.normalized_direction= rocket.speed;
				rocket.normalized_direction.Normalize();
			}
			else if( rocket_description.Auto2 )
			{
				m_Vec3 target_pos;
				if( FindNearestPlayerPos( rocket.previous_position, target_pos ) )
				{
					m_Vec3 dir_to_target= target_pos - rocket.previous_position;
					dir_to_target.Normalize();

					m_Vec3 rot_axis= mVec3Cross( rocket.normalized_direction, dir_to_target );
					const float rot_axis_square_length= rot_axis.SquareLength();
					if( rot_axis_square_length < 0.001f * 0.001f )
						rot_axis= m_Vec3( 0.0f, 0.0f, 1.0f );

					const float c_rot_speed= Constants::half_pi;
					m_Mat4 mat;
					mat.Rotate( rot_axis, last_tick_delta_s * c_rot_speed );

					rocket.normalized_direction= rocket.normalized_direction * mat;
					rocket.normalized_direction.Normalize();
				}

				new_pos= rocket.previous_position + rocket.normalized_direction * speed * last_tick_delta_s;
			}
			else
			{
				new_pos=
					rocket.start_point +
					rocket.normalized_direction * ( time_delta_s * speed ) +
					m_Vec3( 0.0f, 0.0f, -1.0f ) * ( gravity_force * time_delta_s * time_delta_s * 0.5f );
			}

			m_Vec3 dir= new_pos - rocket.previous_position;
			const float max_distance= dir.Length() + c_length_eps;
			dir.Normalize();

			hit_result= ProcessShot( rocket.previous_position, dir, max_distance, rocket.owner_id );

			if( rocket_description.reflect &&
				hit_result.object_type == HitResult::ObjectType::Floor && hit_result.object_index == 0u )
				hit_result.object_type= HitResult::ObjectType::None; // Reflecting rockets does not hit floors.

			// Emit smoke trail
			const unsigned int sprite_effect_id=
				game_resources_->rockets_description[ rocket.rocket_type_id ].smoke_trail_effect_id;
			if( sprite_effect_id != 0u )
			{
				const float c_particels_per_unit= 2.0f; // TODO - calibrate
				const float length_delta= ( new_pos - rocket.previous_position ).Length() * c_particels_per_unit;
				const float new_track_length= rocket.track_length + length_delta;
				for( unsigned int i= static_cast<unsigned int>( rocket.track_length ) + 1u;
					i <= static_cast<unsigned int>( new_track_length ); i++ )
				{
					const float part= ( float(i) - rocket.track_length ) / length_delta;

					sprite_effects_.emplace_back();
					SpriteEffect& effect= sprite_effects_.back();

					effect.pos= ( 1.0f - part ) * rocket.previous_position + part * new_pos;
					effect.effect_id= sprite_effect_id;
				}

				rocket.track_length= new_track_length;
			}

			rocket.previous_position= new_pos;
		}

		// Gen hit effect
		const float c_walls_effect_offset= 1.0f / 32.0f;
		if( hit_result.object_type == HitResult::ObjectType::StaticWall )
		{
			GenParticleEffectForRocketHit(
				hit_result.pos + GetNormalForWall( map_data_->static_walls[ hit_result.object_index ] ) * c_walls_effect_offset,
				rocket.rocket_type_id );
		}
		else if( hit_result.object_type == HitResult::ObjectType::DynamicWall )
		{
			GenParticleEffectForRocketHit(
				hit_result.pos + GetNormalForWall( dynamic_walls_[ hit_result.object_index ] ) * c_walls_effect_offset,
				rocket.rocket_type_id );
		}
		else if( hit_result.object_type == HitResult::ObjectType::Floor )
		{
			GenParticleEffectForRocketHit(
				hit_result.pos + m_Vec3( 0.0f, 0.0f, ( hit_result.object_index == 0 ? 1.0f : -1.0f ) * c_walls_effect_offset ),
				rocket.rocket_type_id );
		}
		else if( hit_result.object_type == HitResult::ObjectType::Model )
			GenParticleEffectForRocketHit( hit_result.pos, rocket.rocket_type_id );
		else if( hit_result.object_type == HitResult::ObjectType::Monster )
		{
			AddParticleEffect( hit_result.pos, ParticleEffect::Blood );

			// Hack for rockets and grenades. Make effect together with blood.
			if( rocket_description.blow_effect == 2 && !has_infinite_speed )
				GenParticleEffectForRocketHit( hit_result.pos, rocket.rocket_type_id );
		}

		// Try break breakable models.
		if( hit_result.object_type == HitResult::ObjectType::Model )
		{
			StaticModel& model= static_models_[ hit_result.object_index ];

			if( model.model_id >= map_data_->models_description.size() )
				goto end_loop;

			const MapData::ModelDescription& model_description= map_data_->models_description[ model.model_id ];

			// Process shot even if model is breakable. TODO - check this.
			ProcessElementLinks(
				MapData::IndexElement::StaticModel,
				hit_result.object_index,
				[&]( const MapData::Link& link )
				{
					if( link.type == MapData::Link::Shoot )
						ProcedureProcessShoot( link.proc_id, current_time );
				} );

			if( model_description.blow_effect != 0 )
			{
				model.health-= int(rocket_description.power);
				if( model.health <= 0 )
				{
					DestroyModel( hit_result.object_index );

					ProcessElementLinks(
						MapData::IndexElement::StaticModel,
						hit_result.object_index,
						[&]( const MapData::Link& link )
						{
							if( link.type == MapData::Link::Destroy )
								ProcedureProcessDestroy( link.proc_id, current_time );
						} );
				}
			}
		}
		else if(
			hit_result.object_type == HitResult::ObjectType::StaticWall ||
			hit_result.object_type == HitResult::ObjectType::DynamicWall )
		{
			ProcessElementLinks(
				hit_result.object_type == HitResult::ObjectType::StaticWall
					? MapData::IndexElement::StaticWall
					: MapData::IndexElement::DynamicWall,
				hit_result.object_index,
				[&]( const MapData::Link& link )
				{
					if( link.type == MapData::Link::Shoot )
						ProcedureProcessShoot( link.proc_id, current_time );
				} );
		}
		else if( hit_result.object_type == HitResult::ObjectType::Floor )
		{
			// TODO - support rockets reflections
		}
		else if( hit_result.object_type == HitResult::ObjectType::Monster )
		{
			auto it= monsters_.find( hit_result.object_index );
			PC_ASSERT( it != monsters_.end() );

			const MonsterBasePtr& monster= it->second;
			PC_ASSERT( monster != nullptr );
			monster->Hit( rocket_description.power, *this, hit_result.object_index ,current_time );
		}

	end_loop:
		// Try remove rocket
		if( hit_result.object_type != HitResult::ObjectType::None || // kill hited
			time_delta_s > 16.0f || // kill old rockets
			has_infinite_speed ) // kill bullets
		{
			if( !has_infinite_speed )
			{
				rockets_death_messages_.emplace_back();
				rockets_death_messages_.back().rocket_id= rocket.rocket_id;
			}

			if( r != rockets_.size() - 1u )
				rockets_[r]= rockets_.back();
			rockets_.pop_back();
		}
		else
			r++;
	} // for rockets

	// Process mines
	for( unsigned int m= 0u; m < mines_.size(); )
	{
		Mine& mine= mines_[m];
		const float time_delta_s= ( current_time - mine.planting_time ).ToSeconds();

		bool need_kill= false;

		if( time_delta_s > 30.0f ) // Kill too old mines
			need_kill= true;
		else if( time_delta_s >= GameConstants::mines_preparation_time_s )
		{
			// Try activate mine.
			bool activated= false;
			for( const MonstersContainer::value_type& monster_value : monsters_ )
			{
				MonsterBase& monster= *monster_value.second;

				const float square_distance= ( monster.Position().xy() - mine.pos.xy() ).SquareLength();
				if( square_distance > 8.0f * 8.0f ) // Too far, early reject.
					continue;

				const float monster_radius=
					monster.MonsterId() == 0u
						? GameConstants::player_radius :
						game_resources_->monsters_description[ monster.MonsterId() ].w_radius;

				const float activation_distance= GameConstants::mines_activation_radius + monster_radius;
				if( square_distance < activation_distance * activation_distance )
					activated= true;
			}

			if( activated )
			{
				need_kill= true;

				// TODO  hit mosnters here

				particles_effects_messages_.emplace_back();
				Messages::ParticleEffectBirth& message= particles_effects_messages_.back();
				message.effect_id= static_cast<unsigned char>(ParticleEffect::Explosion);
				PositionToMessagePosition( mine.pos, message.xyz );

				PlayMapEventSound( mine.pos, 40u );
			}
		}

		if( need_kill )
		{
			dynamic_items_death_messages_.emplace_back();
			dynamic_items_death_messages_.back().item_id= mine.id;

			if( m != mines_.size() - 1u )
				mine= mines_.back();
			mines_.pop_back();
		}
		else
			m++;
	}

	// Process monsters
	for( MonstersContainer::value_type& monster_value : monsters_ )
	{
		monster_value.second->Tick( *this, monster_value.first, current_time, last_tick_delta );

		// Process teleports for monster
		MonsterBase& monster= *monster_value.second;

		const float c_teleport_radius= 0.4f;

		for( const MapData::Teleport& teleport : map_data_->teleports )
		{
			const m_Vec2 tele_pos( float(teleport.from[0]) + 0.5f, float(teleport.from[1]) + 0.5f );

			if( ( tele_pos - monster.Position().xy() ).SquareLength() >= c_teleport_radius * c_teleport_radius )
				continue;

			m_Vec2 dst;
			for( unsigned int j= 0u; j < 2u; j++ )
			{
				if( teleport.to[j] >= MapData::c_map_size )
					dst.ToArr()[j]= float( teleport.to[j] ) / 256.0f;
				else
					dst.ToArr()[j]= float( teleport.to[j] );
			}
			monster.Teleport(
				m_Vec3(
					dst,
					GetFloorLevel( dst, GameConstants::player_radius ) ),
				teleport.angle );
			break;
		}

		// Process wind for monster
		// TODO - select more correct way to do this.
		const int wind_x= static_cast<int>( monster.Position().x - 0.5f );
		const int wind_y= static_cast<int>( monster.Position().y - 0.5f );
		if( wind_x >= 0 && wind_x < int(MapData::c_map_size - 1u) &&
			wind_y >= 0 && wind_y < int(MapData::c_map_size - 1u) )
		{
			// Find interpolated value of wind in 4 cells, nearest to monster center.
			const auto wind_fetch=
			[&]( int x, int y )
			{
				const char* const wind_cell= wind_field_[ x + y * int(MapData::c_map_size) ];
				return m_Vec2( wind_cell[0], wind_cell[1] );
			};
			const float dx= monster.Position().x - 0.5f - float(wind_x);
			const float dy= monster.Position().y - 0.5f - float(wind_y);

			const m_Vec2 wind_vec=
				wind_fetch(wind_x  , wind_y  ) * (1.0f - dx) * (1.0f - dy) +
				wind_fetch(wind_x  , wind_y+1) * (1.0f - dx) *         dy  +
				wind_fetch(wind_x+1, wind_y  ) *         dx  * (1.0f - dy) +
				wind_fetch(wind_x+1, wind_y+1) *         dx  *         dy;

			if( wind_vec.SquareLength() > 0.0f )
			{
				const float time_delta_s= last_tick_delta_s;
				const float c_wind_power_scale= 0.5f;
				const m_Vec2 pos_delta= time_delta_s * c_wind_power_scale * wind_vec;

				monster.SetPosition( monster.Position() + m_Vec3( pos_delta, 0.0f ) );
			}
		}

		// Process death for monster.
		// TODO - make death zone intersection calculation correct, like with wind zones.
		const int monster_x= static_cast<int>( monster.Position().x );
		const int monster_y= static_cast<int>( monster.Position().y );
		if( monster_x >= 0 && monster_x < int(MapData::c_map_size) &&
			monster_y >= 0 && monster_y < int(MapData::c_map_size) )
		{
			const DamageFiledCell& cell= death_field_[ monster_x + monster_y * int(MapData::c_map_size) ];
			if( cell.damage > 0u && death_ticks > 0u )
			{
				// TODO - select correct monster height
				if( !( monster.Position().z > float(cell.z_top) / 64u ||
					   monster.Position().z + GameConstants::player_height < float(cell.z_bottom) / 64u ) )
					monster.Hit( int( cell.damage * death_ticks ), *this, monster_value.first, current_time );
			}
		}
	}

	// Collide monsters with map
	for( MonstersContainer::value_type& monster_value : monsters_ )
	{
		MonsterBase& monster= *monster_value.second;
		const bool is_player= monster.MonsterId() == 0u;

		if( is_player && static_cast<const Player&>(monster).IsNoclip() )
			continue;

		const float height= GameConstants::player_height; // TODO - select height
		const float radius= is_player ? GameConstants::player_radius : game_resources_->monsters_description[ monster.MonsterId() ].w_radius;

		bool on_floor= false;
		const m_Vec3 old_monster_pos= monster.Position();
		const m_Vec3 new_monster_pos=
			CollideWithMap(
				old_monster_pos, height, radius,
				on_floor );

		const m_Vec3 position_delta= new_monster_pos - old_monster_pos;

		if( position_delta.z != 0.0f ) // Vertical clamp
			monster.ClampSpeed( m_Vec3( 0.0f, 0.0f, position_delta.z > 0.0f ? 1.0f : -1.0f ) );

		const float position_delta_length= position_delta.xy().Length();
		if( position_delta_length != 0.0f ) // Horizontal clamp
			monster.ClampSpeed( m_Vec3( position_delta.xy() / position_delta_length, 0.0f ) );

		monster.SetPosition( new_monster_pos );
		monster.SetOnFloor( on_floor );
	}

	// Collide monsters together
	for( MonstersContainer::value_type& first_monster_value : monsters_ )
	{
		MonsterBase& first_monster= *first_monster_value.second;
		if( first_monster.Health() <= 0 )
			continue;

		const float first_monster_radius= game_resources_->monsters_description[ first_monster.MonsterId() ].w_radius;
		const m_Vec2 first_monster_z_minmax=
			first_monster.GetZMinMax() + m_Vec2( first_monster.Position().z, first_monster.Position().z );

		for( MonstersContainer::value_type& second_monster_value : monsters_ )
		{
			MonsterBase& second_monster= *second_monster_value.second;
			if( &second_monster == &first_monster )
				continue;

			if( second_monster.Health() <= 0 )
				continue;

			const float square_distance= ( first_monster.Position().xy() - second_monster.Position().xy() ).SquareLength();

			const float c_max_collide_distance= 8.0f;
			if( square_distance > c_max_collide_distance * c_max_collide_distance )
				continue;

			const float second_monster_radius= game_resources_->monsters_description[ second_monster.MonsterId() ].w_radius;
			const float min_distance= second_monster_radius + first_monster_radius;
			if( square_distance > min_distance * min_distance )
				continue;

			const m_Vec2 second_monster_z_minmax=
				second_monster.GetZMinMax() + m_Vec2( second_monster.Position().z, second_monster.Position().z );
			if(  first_monster_z_minmax.y < second_monster_z_minmax.x ||
				second_monster_z_minmax.y <  first_monster_z_minmax.x ) // Z check
				continue;

			// Collide here
			m_Vec2 collide_vec= second_monster.Position().xy() - first_monster.Position().xy();
			collide_vec.Normalize();

			const float move_delta= min_distance - std::sqrt( square_distance );

			float first_monster_k;
			if( first_monster.MonsterId() == 0u && second_monster.MonsterId() != 0u )
				first_monster_k= 1.0f;
			else if( first_monster.MonsterId() != 0u && second_monster.MonsterId() == 0u )
				first_monster_k= 0.0f;
			else
				first_monster_k= 0.5f;

			const m_Vec2  first_monster_pos=  first_monster.Position().xy() - collide_vec * move_delta * first_monster_k;
			const m_Vec2 second_monster_pos= second_monster.Position().xy() + collide_vec * move_delta * ( 1.0f - first_monster_k );

			 first_monster.SetPosition( m_Vec3( first_monster_pos ,  first_monster.Position().z ) );
			second_monster.SetPosition( m_Vec3( second_monster_pos, second_monster.Position().z ) );
		}
	}

	// At end of this procedure, report about map change, if this needed.
	// Do it here, because map can be desctructed at callback call.
	if( map_end_triggered_ &&
		map_end_callback_ != nullptr )
	{
		map_end_triggered_= false;
		map_end_callback_();
	}
}

void Map::SendMessagesForNewlyConnectedPlayer( MessagesSender& messages_sender ) const
{
	// Send monsters
	for( const MonstersContainer::value_type& monster_entry : monsters_ )
	{
		Messages::MonsterBirth message;

		PrepareMonsterStateMessage( *monster_entry.second, message.initial_state );
		message.initial_state.monster_id= monster_entry.first;
		message.monster_id= monster_entry.first;

		messages_sender.SendReliableMessage( message );
	}
}

void Map::SendUpdateMessages( MessagesSender& messages_sender ) const
{
	Messages::WallPosition wall_message;

	for( const DynamicWall& wall : dynamic_walls_ )
	{
		wall_message.wall_index= &wall - dynamic_walls_.data();

		PositionToMessagePosition( wall.vert_pos[0], wall_message.vertices_xy[0] );
		PositionToMessagePosition( wall.vert_pos[1], wall_message.vertices_xy[1] );
		wall_message.z= CoordToMessageCoord( wall.z );
		wall_message.texture_id= wall.texture_id;

		messages_sender.SendUnreliableMessage( wall_message );
	}

	Messages::StaticModelState model_message;

	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		const StaticModel& model= static_models_[m];

		model_message.static_model_index= m;
		model_message.animation_frame= model.current_animation_frame;
		model_message.animation_playing= model.animation_state == StaticModel::AnimationState::Animation;
		model_message.model_id= model.model_id;
		model_message.visible= !model.picked;

		PositionToMessagePosition( model.pos, model_message.xyz );
		model_message.angle= AngleToMessageAngle( model.angle );

		messages_sender.SendUnreliableMessage( model_message );
	}

	for( const Item& item : items_ )
	{
		Messages::ItemState message;
		message.item_index= &item - items_.data();
		message.z= CoordToMessageCoord( item.pos.z );
		message.picked= item.picked_up;

		messages_sender.SendUnreliableMessage( message );
	}

	Messages::SpriteEffectBirth sprite_message;

	for( const SpriteEffect& effect : sprite_effects_ )
	{
		sprite_message.effect_id= effect.effect_id;
		PositionToMessagePosition( effect.pos, sprite_message.xyz );

		messages_sender.SendUnreliableMessage( sprite_message );
	}

	for( const MonstersContainer::value_type& monster_value : monsters_ )
	{
		Messages::MonsterState monster_message;

		PrepareMonsterStateMessage( *monster_value.second, monster_message );
		monster_message.monster_id= monster_value.first;

		messages_sender.SendUnreliableMessage( monster_message );
	}

	for( const Messages::RocketBirth& message : rockets_birth_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}
	for( const Messages::RocketDeath& message : rockets_death_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}
	for( const Messages::DynamicItemBirth& message : dynamic_items_birth_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}
	for( const Messages::DynamicItemDeath& message : dynamic_items_death_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}
	for( const Messages::ParticleEffectBirth& message : particles_effects_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}
	for( const Messages::MonsterPartBirth& message : monsters_parts_birth_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}
	for( const Messages::MapEventSound& message : map_events_sounds_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}
	for( const Messages::MonsterLinkedSound& message : monster_linked_sounds_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}
	for( const Messages::MonsterSound& message : monsters_sounds_messages_ )
	{
		messages_sender.SendUnreliableMessage( message );
	}

	for( const Rocket& rocket : rockets_ )
	{
		Messages::RocketState rocket_message;

		rocket_message.rocket_id= rocket.rocket_id;
		PositionToMessagePosition( rocket.previous_position, rocket_message.xyz );

		float angle[2];
		VecToAngles( rocket.normalized_direction, angle );
		for( unsigned int j= 0u; j < 2u; j++ )
			rocket_message.angle[j]= AngleToMessageAngle( angle[j] );

		messages_sender.SendUnreliableMessage( rocket_message );
	}
}

void Map::ClearUpdateEvents()
{
	sprite_effects_.clear();
	rockets_birth_messages_.clear();
	rockets_death_messages_.clear();
	dynamic_items_birth_messages_.clear();
	dynamic_items_death_messages_.clear();
	particles_effects_messages_.clear();
	monsters_parts_birth_messages_.clear();
	map_events_sounds_messages_.clear();
	monster_linked_sounds_messages_.clear();
	monsters_sounds_messages_.clear();
}

void Map::ActivateProcedure( const unsigned int procedure_number, const Time current_time )
{
	ProcedureState& procedure_state= procedures_[ procedure_number ];

	procedure_state.movement_stage= 0.0f;
	procedure_state.movement_state= ProcedureState::MovementState::StartWait;
	procedure_state.last_state_change_time= current_time;
}

void Map::TryActivateProcedure(
	unsigned int procedure_number,
	const Time current_time,
	Player& player,
	MessagesSender& messages_sender )
{
	if( !player.TryActivateProcedure( procedure_number, current_time ) )
		return;

	PC_ASSERT( procedure_number < procedures_.size() );

	const MapData::Procedure& procedure= map_data_->procedures[ procedure_number ];
	ProcedureState& procedure_state= procedures_[ procedure_number ];

	const bool have_necessary_keys=
		( !procedure.  red_key_required || player.HaveRedKey() ) &&
		( !procedure.green_key_required || player.HaveGreenKey() ) &&
		( !procedure. blue_key_required || player.HaveBlueKey() );

	if(
		have_necessary_keys &&
		!procedure_state.locked &&
		procedure_state.movement_state == ProcedureState::MovementState::None )
	{
		ActivateProcedure( procedure_number, current_time );
	} // if activated

	// Activation messages.
	if( procedure.first_message_number != 0u &&
		!procedure_state.first_message_printed )
	{
		procedure_state.first_message_printed= true;

		Messages::TextMessage text_message;
		text_message.text_message_number= procedure.first_message_number;
		messages_sender.SendUnreliableMessage( text_message );
	}
	if( procedure.lock_message_number != 0u &&
		( procedure_state.locked || !have_necessary_keys ) )
	{
		Messages::TextMessage text_message;
		text_message.text_message_number= procedure.lock_message_number;
		messages_sender.SendUnreliableMessage( text_message );
	}
	if( procedure.on_message_number != 0u )
	{
		Messages::TextMessage text_message;
		text_message.text_message_number= procedure.on_message_number;
		messages_sender.SendUnreliableMessage( text_message );
	}
}

void Map::ProcedureProcessDestroy( const unsigned int procedure_number, const Time current_time )
{
	ProcedureState& procedure_state= procedures_[ procedure_number ];

	// Autol-unlock locked procedures
	procedure_state.locked= false;

	ActivateProcedure( procedure_number, current_time );
}

void Map::ProcedureProcessShoot( const unsigned int procedure_number, const Time current_time )
{
	PC_ASSERT( procedure_number < procedures_.size() );
	const ProcedureState& procedure_state= procedures_[ procedure_number ];
	if( procedure_state.movement_state != ProcedureState::MovementState::None )
		return;

	// TODO - did this really need?
	if( procedure_state.locked )
		return;

	ActivateProcedure( procedure_number, current_time );
}

void Map::ActivateProcedureSwitches( const MapData::Procedure& procedure, const bool inverse_animation, const Time current_time )
{
	for( const MapData::Procedure::SwitchPos& siwtch_pos : procedure.linked_switches )
	{
		if( siwtch_pos.x >= MapData::c_map_size || siwtch_pos.y >= MapData::c_map_size )
			continue;

		const MapData::IndexElement& index_element= map_data_->map_index[ siwtch_pos.x + siwtch_pos.y * MapData::c_map_size ];
		if( index_element.type == MapData::IndexElement::StaticModel )
		{
			PC_ASSERT( index_element.index < static_models_.size() );
			StaticModel& model= static_models_[ index_element.index ];

			if( model.animation_state == StaticModel::AnimationState::SingleFrame )
			{
				model.animation_start_time= current_time;

				if( inverse_animation )
				{
					model.animation_state= StaticModel::AnimationState::SingleReverseAnimation;
					if( model.model_id < map_data_->models.size() )
						model.animation_start_frame= map_data_->models[ model.model_id ].frame_count - 1u;
					else
						model.animation_start_frame= 0u;
				}
				else
				{
					model.animation_state= StaticModel::AnimationState::SingleAnimation;
					model.animation_start_frame= 0u;
				}
			}
		}
	}
}

void Map::DoProcedureImmediateCommands( const MapData::Procedure& procedure )
{
	// Do immediate commands
	for( const MapData::Procedure::ActionCommand& command : procedure.action_commands )
	{
		using Command= MapData::Procedure::ActionCommandId;
		if( command.id == Command::Lock )
		{
			const unsigned short proc_number= static_cast<unsigned short>( command.args[0] );
			PC_ASSERT( proc_number < procedures_.size() );

			procedures_[ proc_number ].locked= true;
		}
		else if( command.id == Command::Unlock )
		{
			const unsigned short proc_number= static_cast<unsigned short>( command.args[0] );
			PC_ASSERT( proc_number < procedures_.size() );

			procedures_[ proc_number ].locked= false;
		}
		// TODO - know, how animation commands works
		else if( command.id == Command::PlayAnimation )
		{}
		else if( command.id == Command::StopAnimation )
		{}
		else if( command.id == Command::Change )
		{
			const unsigned int x= static_cast<unsigned int>( command.args[0] );
			const unsigned int y= static_cast<unsigned int>( command.args[1] );
			const unsigned int id= static_cast<unsigned int>( command.args[2] );
			if( x < MapData::c_map_size && y < MapData::c_map_size )
			{
				const MapData::IndexElement& index_element= map_data_->map_index[ x + y * MapData::c_map_size ];
				if( index_element.type == MapData::IndexElement::StaticModel )
				{
					PC_ASSERT( index_element.index < static_models_.size() );

					StaticModel& model = static_models_[ index_element.index ];

					// Reset Animation, if model changed.
					if( model.model_id < map_data_->models_description.size())
					{
						if( ACode( map_data_->models_description[ model.model_id ].ac ) == ACode::Switch)
						{
							model.animation_start_frame= 0u;
							model.animation_state= StaticModel::AnimationState::SingleFrame;
						}
					}
					else
					{
						model.animation_start_frame= 0;
						model.animation_state= StaticModel::AnimationState::Animation;
					}

					model.model_id= id - 163u;
				}
				else if( index_element.type == MapData::IndexElement::DynamicWall )
				{
					PC_ASSERT( index_element.index < dynamic_walls_.size() );
					dynamic_walls_[ index_element.index ].texture_id= id;
				}
			}
		}
		else if( command.id == Command::Wind )
			ProcessWind( command, true );
		else if( command.id == Command::Death )
			ProcessDeathZone( command, true );
		else if( command.id == Command::Explode )
		{
			const unsigned int x= static_cast<unsigned int>( command.args[0] );
			const unsigned int y= static_cast<unsigned int>( command.args[1] );

			if( x < MapData::c_map_size && y < MapData::c_map_size )
			{
				const MapData::IndexElement& index_element= map_data_->map_index[ x + y * MapData::c_map_size ];
				if( index_element.type == MapData::IndexElement::StaticModel )
					DestroyModel( index_element.index );
			}
		}
		// TODO - process other commands
		else
		{}
	}
}

void Map::DoProcedureDeactivationCommands( const MapData::Procedure& procedure )
{
	using Command= MapData::Procedure::ActionCommandId;

	// Check nonstop.
	// TODO - set nonstop as procedure flag, not as action command.
	for( const MapData::Procedure::ActionCommand& command : procedure.action_commands )
		if( command.id == Command::Nonstop )
			return;

	for( const MapData::Procedure::ActionCommand& command : procedure.action_commands )
	{
		if( command.id == Command::Wind )
			ProcessWind( command, false );
		else if( command.id == Command::Death )
			ProcessDeathZone( command, false );
	}
}

void Map::ReturnProcedure( const unsigned int procedure_number, const Time current_time )
{
	PC_ASSERT( procedure_number < map_data_->procedures.size() );

	const MapData::Procedure& procededure= map_data_->procedures[ procedure_number ];
	ProcedureState& procedure_state= procedures_[ procedure_number ];

	if( procedure_state.locked )
		return;

	switch( procedure_state.movement_state )
	{
	case ProcedureState::MovementState::None:
	case ProcedureState::MovementState::ReverseMovement:
		break;

	case ProcedureState::MovementState::StartWait:
		procedure_state.movement_state= ProcedureState::MovementState::None;
		break;

	case ProcedureState::MovementState::Movement:
	{
		procedure_state.movement_state= ProcedureState::MovementState::ReverseMovement;
		float dt_s;
		if( procededure.speed > 0.0f )
			dt_s=
				std::max(
					1.0f / ( procededure.speed * GameConstants::procedures_speed_scale ) -
					( current_time - procedure_state.last_state_change_time ).ToSeconds(),
					0.0f );
		else
			dt_s= 0.0f;
		procedure_state.last_state_change_time= current_time - Time::FromSeconds(dt_s);
	}
		break;

	case ProcedureState::MovementState::BackWait:
		procedure_state.movement_state= ProcedureState::MovementState::ReverseMovement;
		procedure_state.last_state_change_time= current_time;
		break;
	};
}

void Map::ProcessWind( const MapData::Procedure::ActionCommand& command, bool activate )
{
	PC_ASSERT( command.id == MapData::Procedure::ActionCommandId::Wind );

	const unsigned int x0= static_cast<unsigned int>( command.args[0] );
	const unsigned int y0= static_cast<unsigned int>( command.args[1] );
	const unsigned int x1= static_cast<unsigned int>( command.args[2] );
	const unsigned int y1= static_cast<unsigned int>( command.args[3] );
	const int dir_x= static_cast<int>( command.args[4] );
	const int dir_y= static_cast<int>( command.args[5] );

	for( unsigned int y= y0; y <= y1 && y < MapData::c_map_size; y++ )
	for( unsigned int x= x0; x <= x1 && x < MapData::c_map_size; x++ )
	{
		char* const cell= wind_field_[ x + y * MapData::c_map_size ];
		if( activate )
		{
			cell[0]= dir_x;
			cell[1]= dir_y;
		}
		else
			cell[0]= cell[1]= 0;
	}
}

void Map::ProcessDeathZone( const MapData::Procedure::ActionCommand& command, const bool activate )
{
	PC_ASSERT( command.id == MapData::Procedure::ActionCommandId::Death );

	const unsigned int x0= static_cast<unsigned int>( command.args[0] );
	const unsigned int y0= static_cast<unsigned int>( command.args[1] );
	const unsigned int x1= static_cast<unsigned int>( command.args[2] );
	const unsigned int y1= static_cast<unsigned int>( command.args[3] );
	const int z_0= static_cast<int>( command.args[4] );
	const int z_1= static_cast<int>( command.args[5] );
	const unsigned char damage= static_cast<unsigned char>( command.args[6] );

	for( unsigned int y= y0; y <= y1 && y < MapData::c_map_size; y++ )
	for( unsigned int x= x0; x <= x1 && x < MapData::c_map_size; x++ )
	{
		DamageFiledCell& cell= death_field_[ x + y * MapData::c_map_size ];
		if( activate )
		{
			cell.damage= damage;
			cell.z_bottom= std::max( std::min( z_0, 255 ), 0 );
			cell.z_top   = std::max( std::min( z_1, 255 ), 0 );
		}
		else
			cell.damage= 0u;
	}
}

void Map::DestroyModel( const unsigned int model_index )
{
	PC_ASSERT( model_index < static_models_.size() );
	StaticModel& model= static_models_[ model_index ];

	EmitModelDestructionEffects( model_index );

	model.model_id++; // now, this model has other model type
	if( model.model_id < map_data_->models_description.size() )
		model.health= map_data_->models_description[ model.model_id ].break_limit;
	else
		model.health= 0;
}

void Map::MoveMapObjects()
{
	// Zero objects transformations.
	for( DynamicWall& wall : dynamic_walls_ )
		wall.transformation.Clear();

	for( StaticModel& model : static_models_ )
	{
		model.transformation.Clear();
		model.transformation_angle_delta= 0.0f;
	}

	/* Accumulate transformations from procedures on objects.
	 * Several transformations can be applied for one object.
	 * But, if transformation effect depends on their order, result may be incorrect.
	 * Examples of "bad" transformations combination:
	 * Rotate + Move, Rotate + Rotate with different center, etc.
	 */
	for( unsigned int p= 0u; p < procedures_.size(); p++ )
	{
		const MapData::Procedure& procedure= map_data_->procedures[p];
		const ProcedureState& procedure_state= procedures_[p];

		float absolute_action_stage;
		if( procedure_state.movement_state == ProcedureState::MovementState::Movement )
			absolute_action_stage= procedure_state.movement_stage;
		else if( procedure_state.movement_state == ProcedureState::MovementState::BackWait )
			absolute_action_stage= 1.0f;
		else if( procedure_state.movement_state == ProcedureState::MovementState::ReverseMovement )
			absolute_action_stage= 1.0f - procedure_state.movement_stage;
		else
			absolute_action_stage= 0.0f;

		for( const MapData::Procedure::ActionCommand& command : procedure.action_commands )
		{
			using Action= MapData::Procedure::ActionCommandId;
			switch( command.id )
			{
			case Action::Move:
			case Action::XMove:
			case Action::YMove:
			{
				const unsigned char x= static_cast<unsigned char>(command.args[0]);
				const unsigned char y= static_cast<unsigned char>(command.args[1]);
				const float dx= command.args[2] * g_commands_coords_scale;
				const float dy= command.args[3] * g_commands_coords_scale;
				const float sound_number= command.args[4];
				PC_UNUSED(sound_number);

				// TODO - maybe fractions depends on way length?
				//const float total_way_length= std::abs(dx) + std::abs(dy);
				const float x_fraction= 0.5f;//std::abs(dx) / total_way_length;
				const float y_fraction= 0.5f;//std::abs(dy) / total_way_length;

				m_Vec2 d_pos( 0.0f, 0.0f );
				if( command.id == Action::XMove )
				{
					if( absolute_action_stage <= x_fraction )
						d_pos.x+= dx * absolute_action_stage / x_fraction;
					else
					{
						d_pos.x+= dx;
						d_pos.y+= dy * ( absolute_action_stage - x_fraction ) / y_fraction;
					}
				}
				else if( command.id == Action::YMove )
				{
					if( absolute_action_stage <= y_fraction )
						d_pos.y+= dy * absolute_action_stage / y_fraction;
					else
					{
						d_pos.x+= dx * ( absolute_action_stage - y_fraction ) / x_fraction;
						d_pos.y+= dy;
					}
				}
				else//if( command.id == Action::Move )
				{
					d_pos.x+= dx * absolute_action_stage;
					d_pos.y+= dy * absolute_action_stage;
				}

				m_Mat3 mat;
				mat.Translate( d_pos );

				PC_ASSERT( x < MapData::c_map_size && y < MapData::c_map_size );
				const MapData::IndexElement& index_element= map_data_->map_index[ x + y * MapData::c_map_size ];

				if( index_element.type == MapData::IndexElement::DynamicWall )
				{
					PC_ASSERT( index_element.index < map_data_->dynamic_walls.size() );
					DynamicWall& wall= dynamic_walls_[ index_element.index ];
					wall.transformation.mat= wall.transformation.mat * mat;
				}
				else if( index_element.type == MapData::IndexElement::StaticModel )
				{
					PC_ASSERT( index_element.index < static_models_.size() );
					StaticModel& model= static_models_[ index_element.index ];
					model.transformation.mat= model.transformation.mat * mat;
				}
			}
				break;

			case Action::Rotate:
			{
				const unsigned char x= static_cast<unsigned char>(command.args[0]);
				const unsigned char y= static_cast<unsigned char>(command.args[1]);
				const float center_x= command.args[2] * g_commands_coords_scale;
				const float center_y= command.args[3] * g_commands_coords_scale;
				const float angle= command.args[4] * Constants::to_rad;
				const float sound_number= command.args[5];
				PC_UNUSED(sound_number);

				const m_Vec2 center( center_x, center_y );
				const float angle_delta= angle * absolute_action_stage;

				m_Mat3 shift, rot, back_shift, mat;
				shift.Translate( -center );
				rot.RotateZ( angle_delta );
				back_shift.Translate( center );
				mat= shift * rot * back_shift;

				PC_ASSERT( x < MapData::c_map_size && y < MapData::c_map_size );
				const MapData::IndexElement& index_element= map_data_->map_index[ x + y * MapData::c_map_size ];

				if( index_element.type == MapData::IndexElement::DynamicWall )
				{
					PC_ASSERT( index_element.index < map_data_->dynamic_walls.size() );
					DynamicWall& wall= dynamic_walls_[ index_element.index ];
					wall.transformation.mat= wall.transformation.mat * mat;
				}
				else if( index_element.type == MapData::IndexElement::StaticModel )
				{
					PC_ASSERT( index_element.index < static_models_.size() );
					StaticModel& model= static_models_[ index_element.index ];
					model.transformation.mat= model.transformation.mat * mat;
					model.transformation_angle_delta+= angle_delta;
				}
			}
				break;

			case Action::Up:
			{
				const unsigned char x= static_cast<unsigned char>(command.args[0]);
				const unsigned char y= static_cast<unsigned char>(command.args[1]);
				const float height= command.args[2] * g_commands_coords_scale * 4.0f;
				const float sound_number= command.args[3];
				PC_UNUSED(sound_number);

				PC_ASSERT( x < MapData::c_map_size && y < MapData::c_map_size );
				const MapData::IndexElement& index_element= map_data_->map_index[ x + y * MapData::c_map_size ];

				const float dz= height * absolute_action_stage;

				if( index_element.type == MapData::IndexElement::DynamicWall )
				{
					PC_ASSERT( index_element.index < map_data_->dynamic_walls.size() );
					DynamicWall& wall= dynamic_walls_[ index_element.index ];
					wall.transformation.d_z+= dz;
				}
				else if( index_element.type == MapData::IndexElement::StaticModel )
				{
					PC_ASSERT( index_element.index < static_models_.size() );
					StaticModel& model= static_models_[ index_element.index ];
					model.transformation.d_z+= dz;
				}
			}
				break;

			default:
				// TODO
				break;
			}
		} // for action commands
	} // for procedures

	// Apply objects transformations.
	for( unsigned int w= 0u; w < dynamic_walls_.size(); w++ )
	{
		const MapData::Wall& map_wall= map_data_->dynamic_walls[ w ];
		DynamicWall& wall= dynamic_walls_[ w ];

		for( unsigned int j= 0u; j < 2u; j++ )
			wall.vert_pos[j]= map_wall.vert_pos[j] * wall.transformation.mat;

		wall.z= wall.transformation.d_z;
	}

	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		const MapData::StaticModel& map_model= map_data_->static_models[ m ];
		StaticModel& model= static_models_[ m ];

		const m_Vec2 xy= map_model.pos * model.transformation.mat;
		model.pos.x= xy.x;
		model.pos.y= xy.y;
		model.pos.z= model.baze_z + model.transformation.d_z;

		model.angle= map_model.angle + model.transformation_angle_delta;
	}
}

Map::HitResult Map::ProcessShot(
	const m_Vec3& shot_start_point,
	const m_Vec3& shot_direction_normalized,
	const float max_distance,
	const EntityId skip_monster_id ) const
{
	HitResult result;
	float nearest_shot_point_square_distance= max_distance * max_distance;

	const auto process_candidate_shot_pos=
	[&]( const m_Vec3& candidate_pos, const HitResult::ObjectType object_type, const unsigned int object_index )
	{
		const float square_distance= ( candidate_pos - shot_start_point ).SquareLength();
		if( square_distance < nearest_shot_point_square_distance )
		{
			result.pos= candidate_pos;
			nearest_shot_point_square_distance= square_distance;

			result.object_type= object_type;
			result.object_index= object_index;
		}
	};

	const auto func=
	[&]( const MapData::IndexElement& element ) -> bool
	{
		if( element.type == MapData::IndexElement::StaticWall )
		{
			PC_ASSERT( element.index < map_data_->static_walls.size() );
			const MapData::Wall& wall= map_data_->static_walls[ element.index ];

			const MapData::WallTextureDescription& wall_texture= map_data_->walls_textures[ wall.texture_id ];
			if( wall_texture.gso[1] )
				goto end;

			m_Vec3 candidate_pos;
			if( RayIntersectWall(
					wall.vert_pos[0], wall.vert_pos[1],
					0.0f, 2.0f,
					shot_start_point, shot_direction_normalized,
					candidate_pos ) )
			{
				process_candidate_shot_pos( candidate_pos, HitResult::ObjectType::StaticWall, &wall - map_data_->static_walls.data() );
			}
		}
		else if( element.type == MapData::IndexElement::StaticModel )
		{
			PC_ASSERT( element.index < static_models_.size() );
			const StaticModel& model= static_models_[ element.index ];

			if( model.model_id >= map_data_->models_description.size() )
				goto end;

			const MapData::ModelDescription& model_description= map_data_->models_description[ model.model_id ];
			if( model_description.radius <= 0.0f )
				goto end;

			const Model& model_data= map_data_->models[ model.model_id ];

			m_Vec3 candidate_pos;
			if( RayIntersectCylinder(
					model.pos.xy(), model_description.radius,
					model_data.z_min + model.pos.z,
					model_data.z_max + model.pos.z,
					shot_start_point, shot_direction_normalized,
					candidate_pos ) )
			{
				process_candidate_shot_pos( candidate_pos, HitResult::ObjectType::Model, &model - static_models_.data() );
			}
		}
		else
		{
			// TODO
		}

		end:
		// TODO - return true, sometimes.
		return false;
	};

	collision_index_.RayCast(
		shot_start_point, shot_direction_normalized,
		func,
		max_distance );

	// Dynamic walls
	for( const DynamicWall& wall : dynamic_walls_ )
	{
		const MapData::WallTextureDescription& wall_texture= map_data_->walls_textures[ wall.texture_id ];
		if( wall_texture.gso[1] )
			continue;

		m_Vec3 candidate_pos;
		if( RayIntersectWall(
				wall.vert_pos[0], wall.vert_pos[1],
				wall.z, wall.z + 2.0f,
				shot_start_point, shot_direction_normalized,
				candidate_pos ) )
		{
			process_candidate_shot_pos( candidate_pos, HitResult::ObjectType::DynamicWall, &wall - dynamic_walls_.data() );
		}
	}

	// Monsters
	for( const MonstersContainer::value_type& monster_value : monsters_ )
	{
		if( monster_value.first == skip_monster_id )
			continue;

		m_Vec3 candidate_pos;
		if( monster_value.second->TryShot(
				shot_start_point, shot_direction_normalized,
				candidate_pos ) )
		{
			process_candidate_shot_pos(
				candidate_pos, HitResult::ObjectType::Monster,
				monster_value.first );
		}
	}

	// Floors, ceilings
	for( unsigned int z= 0u; z <= 2u; z+= 2u )
	{
		m_Vec3 candidate_pos;
		if( RayIntersectXYPlane(
				float(z),
				shot_start_point, shot_direction_normalized,
				candidate_pos ) )
		{
			const int x= static_cast<int>( std::floor(candidate_pos.x) );
			const int y= static_cast<int>( std::floor(candidate_pos.y) );
			if( x < 0 || x >= int(MapData::c_map_size) ||
				y < 0 || y >= int(MapData::c_map_size) )
				continue;

			const int coord= x + y * int(MapData::c_map_size);
			const unsigned char texture_id=
				( z == 0 ? map_data_->floor_textures : map_data_->ceiling_textures )[ coord ];

			if( texture_id == MapData::c_empty_floor_texture_id ||
				texture_id == MapData::c_sky_floor_texture_id )
				continue;

			process_candidate_shot_pos( candidate_pos, HitResult::ObjectType::Floor, z >> 1u );
		}
	}

	return result;
}

bool Map::FindNearestPlayerPos( const m_Vec3& pos, m_Vec3& out_pos ) const
{
	if( players_.empty() )
		return false;

	float min_square_distance= Constants::max_float;
	m_Vec3 nearest_player_pos;

	const m_Vec2 z_minmax= players_.begin()->second->GetZMinMax();
	const float dz= ( z_minmax.x + z_minmax.y ) * 0.5f;

	for( const PlayersContainer::value_type& player_value : players_ )
	{
		const Player& player= *player_value.second;
		const m_Vec3 player_pos_corrected= m_Vec3( player.Position().x, player.Position().y, player.Position().z + dz );

		const float square_distance= ( player_pos_corrected - pos ).SquareLength();
		if( square_distance < min_square_distance )
		{
			nearest_player_pos= player_pos_corrected;
			min_square_distance= square_distance;
		}
	}

	out_pos= nearest_player_pos;
	return true;
}

float Map::GetFloorLevel( const m_Vec2& pos, const float radius ) const
{
	float max_dz= 0.0f;

	const float c_max_floor_level= 1.2f;

	for( unsigned int m= 0u; m < static_models_.size(); m++ )
	{
		const MapData::StaticModel& map_model= map_data_->static_models[m];
		if( map_model.is_dynamic )
			continue;

		if( map_model.model_id >= map_data_->models_description.size() )
			continue;

		const MapData::ModelDescription& model_description= map_data_->models_description[ map_model.model_id ];
		if( model_description.ac != 0u )
			continue;

		const float model_radius= model_description.radius;
		if( model_radius <= 0.0f )
			continue;

		const float square_distance= ( pos - map_model.pos ).SquareLength();
		const float collision_distance= model_radius + radius;
		if( square_distance > collision_distance * collision_distance )
			continue;

		// Hit here
		const Model& model= map_data_->models[ map_model.model_id ];

		if( model.z_max >= c_max_floor_level )
			continue;

		max_dz= std::max( max_dz, model.z_max );
	}

	return max_dz;
}

EntityId Map::GetNextMonsterId()
{
	return ++next_monster_id_;
}

void Map::PrepareMonsterStateMessage( const MonsterBase& monster, Messages::MonsterState& message )
{
	PositionToMessagePosition( monster.Position(), message.xyz );
	message.angle= AngleToMessageAngle( monster.Angle() );
	message.monster_type= monster.MonsterId();
	message.body_parts_mask= monster.GetBodyPartsMask();
	message.animation= monster.CurrentAnimation();
	message.animation_frame= monster.CurrentAnimationFrame();
}

void Map::EmitModelDestructionEffects( const unsigned int model_number )
{
	PC_ASSERT( model_number < static_models_.size() );
	const StaticModel& model= static_models_[ model_number ];

	if( model.model_id >= map_data_->models_description.size() )
		return;

	const MapData::ModelDescription& description= map_data_->models_description[ model.model_id ];
	const Model& model_data= map_data_->models[ model.model_id ];

	const unsigned int blow_effect_id= description.blow_effect % 100u;

	m_Vec3 pos= model.pos;
	// TODO - tune this formula. It can be invalid.
	pos.z+= ( model_data.z_min + model_data.z_max ) * 0.5f + float( description.bmpz ) / 128.0f;

	particles_effects_messages_.emplace_back();
	Messages::ParticleEffectBirth& message= particles_effects_messages_.back();

	PositionToMessagePosition( pos, message.xyz );
	message.effect_id= static_cast<unsigned char>( ParticleEffect::FirstBlowEffect ) + blow_effect_id;

	if( description.break_sfx_number != 0 )
		PlayMapEventSound( pos, description.break_sfx_number );
}

void Map::AddParticleEffect( const m_Vec3& pos, const ParticleEffect particle_effect )
{
	particles_effects_messages_.emplace_back();
	Messages::ParticleEffectBirth& message= particles_effects_messages_.back();

	PositionToMessagePosition( pos, message.xyz );
	message.effect_id= static_cast<unsigned char>( particle_effect );
}

void Map::GenParticleEffectForRocketHit( const m_Vec3& pos, const unsigned int rocket_type_id )
{
	PC_ASSERT( rocket_type_id < game_resources_->rockets_description.size() );
	const GameResources::RocketDescription& description= game_resources_->rockets_description[ rocket_type_id ];

	Messages::ParticleEffectBirth* message= nullptr;

	if( description.model_file_name[0] == '\0' )
	{ // bullet
		if( description.blow_effect == 1 )
		{
			//bullet
			particles_effects_messages_.emplace_back();
			message= & particles_effects_messages_.back();
			message->effect_id= static_cast<unsigned char>( ParticleEffect::Bullet );
		}
	}
	else
	{
		if( description.blow_effect == 1 || description.blow_effect == 3 || description.blow_effect == 4 )
		{
			// sparcles
			particles_effects_messages_.emplace_back();
			message= & particles_effects_messages_.back();
			message->effect_id= static_cast<unsigned char>( ParticleEffect::Sparkles );
		}
		if( description.blow_effect == 2 )
		{
			//explosion
			particles_effects_messages_.emplace_back();
			message= & particles_effects_messages_.back();
			message->effect_id= static_cast<unsigned char>( ParticleEffect::Explosion );
		}
		if( description.blow_effect == 4 )
		{
			// Mega destroyer flash - TODO
		}
	}

	if( message != nullptr )
		PositionToMessagePosition( pos, message->xyz );

	PlayMapEventSound( pos, Sound::SoundId::FirstRocketHit + rocket_type_id );
}

} // PanzerChasm
