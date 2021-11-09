#include "dialogue.h" // IWYU pragma: associated

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "activity_type.h"
#include "auto_pickup.h"
#include "avatar.h"
#include "calendar.h"
#include "cata_utility.h"
#include "catacharset.h"
#include "character.h"
#include "character_id.h"
#include "clzones.h"
#include "color.h"
#include "computer.h"
#include "condition.h"
#include "coordinates.h"
#include "creature_tracker.h"
#include "debug.h"
#include "effect_on_condition.h"
#include "enums.h"
#include "faction.h"
#include "faction_camp.h"
#include "game.h"
#include "game_constants.h"
#include "generic_factory.h"
#include "help.h"
#include "input.h"
#include "item.h"
#include "item_category.h"
#include "item_pocket.h"
#include "itype.h"
#include "json.h"
#include "line.h"
#include "magic.h"
#include "map.h"
#include "mapgen_functions.h"
#include "martialarts.h"
#include "messages.h"
#include "mission.h"
#include "mtype.h"
#include "npc.h"
#include "npctalk.h"
#include "npctrade.h"
#include "output.h"
#include "overmapbuffer.h"
#include "pimpl.h"
#include "player_activity.h"
#include "point.h"
#include "popup.h"
#include "recipe.h"
#include "recipe_groups.h"
#include "ret_val.h"
#include "rng.h"
#include "skill.h"
#include "sounds.h"
#include "string_formatter.h"
#include "string_input_popup.h"
#include "talker.h"
#include "text_snippets.h"
#include "timed_event.h"
#include "translations.h"
#include "ui.h"
#include "ui_manager.h"
#include "veh_type.h"
#include "vehicle.h"
#include "vpart_position.h"
#include "vpart_range.h"

static const activity_id ACT_AIM( "ACT_AIM" );
static const activity_id ACT_SOCIALIZE( "ACT_SOCIALIZE" );
static const activity_id ACT_TRAIN( "ACT_TRAIN" );
static const activity_id ACT_WAIT_NPC( "ACT_WAIT_NPC" );

static const efftype_id effect_asked_to_train( "asked_to_train" );
static const efftype_id effect_narcosis( "narcosis" );
static const efftype_id effect_riding( "riding" );
static const efftype_id effect_sleep( "sleep" );
static const efftype_id effect_under_operation( "under_operation" );

static const itype_id fuel_type_animal( "animal" );
static const itype_id itype_foodperson_mask( "foodperson_mask" );
static const itype_id itype_foodperson_mask_on( "foodperson_mask_on" );

static const skill_id skill_firstaid( "firstaid" );

static const skill_id skill_speech( "speech" );

static const trait_id trait_DEBUG_MIND_CONTROL( "DEBUG_MIND_CONTROL" );
static const trait_id trait_PROF_FOODP( "PROF_FOODP" );

static const zone_type_id zone_type_NPC_INVESTIGATE_ONLY( "NPC_INVESTIGATE_ONLY" );
static const zone_type_id zone_type_NPC_NO_INVESTIGATE( "NPC_NO_INVESTIGATE" );

static std::map<std::string, json_talk_topic> json_talk_topics;

#define dbg(x) DebugLog((x),D_GAME) << __FILE__ << ":" << __LINE__ << ": "

static int topic_category( const talk_topic &the_topic );

static const talk_topic &special_talk( const std::string &action );

static bool friendly_teacher( const Character &student, const Character &teacher )
{
    return ( student.is_npc() && teacher.is_avatar() ) ||
           ( teacher.is_npc() && teacher.as_npc()->is_player_ally() );
}

static tripoint get_tripoint_from_var( talker *target, cata::optional<std::string> target_var,
                                       bool global )
{
    tripoint target_pos = get_map().getabs( target->pos() );
    if( target_var.has_value() ) {
        std::string value;
        if( global ) {
            global_variables &globvars = get_globals();
            value = globvars.get_global_value( target_var.value() );
        } else {
            value = target->get_value( target_var.value() );
        }
        if( !value.empty() ) {
            target_pos = tripoint::from_string( value );
        }
    }
    return target_pos;
}

std::string talk_trial::name() const
{
    static const std::array<std::string, NUM_TALK_TRIALS> texts = { {
            "", translate_marker( "LIE" ), translate_marker( "PERSUADE" ), translate_marker( "INTIMIDATE" ), ""
        }
    };
    if( static_cast<size_t>( type ) >= texts.size() ) {
        debugmsg( "invalid trial type %d", static_cast<int>( type ) );
        return std::string();
    }
    return texts[type].empty() ? std::string() : _( texts[type] );
}

time_duration calc_skill_training_time( const npc &p, const skill_id &skill )
{
    return calc_skill_training_time_char( p, get_player_character(), skill );
}

/** Time (in turns) and cost (in cent) for training: */
time_duration calc_skill_training_time_char( const Character &teacher, const Character &student,
        const skill_id &skill )
{
    return 1_hours + 30_minutes * student.get_skill_level( skill ) -
           1_minutes * teacher.get_skill_level( skill );
}

int calc_skill_training_cost( const npc &p, const skill_id &skill )
{
    return calc_skill_training_cost_char( p, get_player_character(), skill );
}

int calc_skill_training_cost_char( const Character &teacher, const Character &student,
                                   const skill_id &skill )
{
    if( friendly_teacher( student, teacher ) ) {
        return 0;
    }
    int skill_level = student.get_knowledge_level( skill );
    return 1000 * ( 1 + skill_level ) * ( 1 + skill_level );
}

time_duration calc_proficiency_training_time( const proficiency_id &proficiency )
{
    const Character &c = get_player_character();
    return calc_proficiency_training_time( c, c, proficiency );
}

time_duration calc_proficiency_training_time( const Character &, const Character &student,
        const proficiency_id &proficiency )
{
    return std::min( 30_minutes, student.proficiency_training_needed( proficiency ) );
}

int calc_proficiency_training_cost( const Character &teacher, const Character &student,
                                    const proficiency_id &proficiency )
{
    if( friendly_teacher( student, teacher ) ) {
        return 0;
    }
    return to_seconds<int>( calc_proficiency_training_time( proficiency ) );
}

int calc_proficiency_training_cost( const npc &p, const proficiency_id &proficiency )
{
    return calc_proficiency_training_cost( p, get_player_character(), proficiency );
}


time_duration calc_ma_style_training_time( const npc &p, const matype_id &id )
{
    return calc_ma_style_training_time( p, get_player_character(), id );
}

// TODO: all styles cost the same and take the same time to train,
// maybe add values to the ma_style class to makes this variable
// TODO: maybe move this function into the ma_style class? Or into the NPC class?
time_duration calc_ma_style_training_time( const Character &, const Character &,
        const matype_id & )
{
    return 30_minutes;
}

int calc_ma_style_training_cost( const npc &p, const matype_id &id )
{
    return calc_ma_style_training_cost( p, get_player_character(), id );
}

int calc_ma_style_training_cost( const Character &teacher, const Character &student,
                                 const matype_id & )
{
    if( friendly_teacher( student, teacher ) ) {
        return 0;
    }
    return 800;
}

// quicker to learn with instruction as opposed to books.
// if this is a known spell, then there is a set time to gain some exp.
// if player doesn't know this spell, then the NPC will teach all of it
// which takes max 6 hours, min 3 hours.
// TODO: a system for NPCs to train new stuff in bits and pieces
// and remember the progress.
time_duration calc_spell_training_time( const Character &, const Character &student,
                                        const spell_id &id )
{
    if( student.magic->knows_spell( id ) ) {
        return 1_hours;
    } else {
        const int time_int = student.magic->time_to_learn_spell( student, id ) / 50;
        return time_duration::from_seconds( clamp( time_int, 7200, 21600 ) );
    }
}

int npc::calc_spell_training_cost( const bool knows, int difficulty, int level )
{
    if( is_player_ally() ) {
        return 0;
    }
    return ::calc_spell_training_cost_gen( knows, difficulty, level );
}

int calc_spell_training_cost( const Character &teacher, const Character &student,
                              const spell_id &id )
{
    if( friendly_teacher( student, teacher ) ) {
        return 0;
    }
    const spell &temp_spell = teacher.magic->get_spell( id );
    const bool knows = student.magic->knows_spell( id );
    return calc_spell_training_cost_gen( knows, temp_spell.get_difficulty(),
                                         temp_spell.get_level() );
}

int calc_spell_training_cost_gen( const bool knows, int difficulty, int level )
{
    int ret = ( 100 * std::max( 1, difficulty ) * std::max( 1, level ) );
    if( !knows ) {
        ret = ret * 2;
    }
    return ret;
}

// Rescale values from "mission scale" to "opinion scale"
int npc_trading::cash_to_favor( const npc &, int cash )
{
    // TODO: It should affect different NPCs to a different degree
    // Square root of mission value in dollars
    // ~31 for zed mom, 50 for horde master, ~63 for plutonium cells
    double scaled_mission_val = std::sqrt( cash / 100.0 );
    return roll_remainder( scaled_mission_val );
}

enum npc_chat_menu {
    NPC_CHAT_DONE,
    NPC_CHAT_TALK,
    NPC_CHAT_YELL,
    NPC_CHAT_START_SEMINAR,
    NPC_CHAT_SENTENCE,
    NPC_CHAT_GUARD,
    NPC_CHAT_FOLLOW,
    NPC_CHAT_AWAKE,
    NPC_CHAT_MOUNT,
    NPC_CHAT_DISMOUNT,
    NPC_CHAT_DANGER,
    NPC_CHAT_ORDERS,
    NPC_CHAT_NO_GUNS,
    NPC_CHAT_PULP,
    NPC_CHAT_FOLLOW_CLOSE,
    NPC_CHAT_MOVE_FREELY,
    NPC_CHAT_SLEEP,
    NPC_CHAT_FORBID_ENGAGE,
    NPC_CHAT_CLEAR_OVERRIDES,
    NPC_CHAT_ANIMAL_VEHICLE_FOLLOW,
    NPC_CHAT_ANIMAL_VEHICLE_STOP_FOLLOW,
    NPC_CHAT_COMMAND_MAGIC_VEHICLE_FOLLOW,
    NPC_CHAT_COMMAND_MAGIC_VEHICLE_STOP_FOLLOW
};

// given a vector of NPCs, presents a menu to allow a player to pick one.
// everyone == true adds another entry at the end to allow selecting all listed NPCs
// this implies a return value of npc_list.size() means "everyone"
static int npc_select_menu( const std::vector<npc *> &npc_list, const std::string &prompt,
                            const bool everyone = true )
{
    if( npc_list.empty() ) {
        return -1;
    }
    const int npc_count = npc_list.size();
    if( npc_count == 1 ) {
        return 0;
    } else {
        uilist nmenu;
        nmenu.text = prompt;
        for( const npc *elem : npc_list ) {
            nmenu.addentry( -1, true, MENU_AUTOASSIGN, elem->name_and_activity() );
        }
        if( npc_count > 1 && everyone ) {
            nmenu.addentry( -1, true, MENU_AUTOASSIGN, _( "Everyone" ) );
        }
        nmenu.query();
        return nmenu.ret;
    }

}

static std::vector<int> npcs_select_menu( const std::vector<npc *> &npc_list,
        const std::string &prompt,
        std::function<bool( const npc * )> exclude_func = nullptr )
{
    std::vector<int> picked;
    if( npc_list.empty() ) {
        return picked;
    }
    const int npc_count = npc_list.size();
    do {
        uilist nmenu;
        nmenu.text = prompt;
        for( int i = 0; i < npc_count; i++ ) {
            std::string entry;
            if( std::find( picked.begin(), picked.end(), i ) != picked.end() ) {
                entry = "* ";
            }
            bool enable = exclude_func == nullptr || !exclude_func( npc_list[i] );
            entry += npc_list[i]->name_and_activity();
            nmenu.addentry( i, enable, MENU_AUTOASSIGN, entry );
        }
        nmenu.addentry( npc_count, true, MENU_AUTOASSIGN, _( "Finish selection" ) );
        nmenu.query();
        if( nmenu.ret < 0 ) {
            return std::vector<int>();
        } else if( nmenu.ret >= npc_count ) {
            break;
        }
        std::vector<int>::iterator exists = std::find( picked.begin(), picked.end(), nmenu.ret );
        if( exists != picked.end() ) {
            picked.erase( exists );
        } else {
            picked.push_back( nmenu.ret );
        }
    } while( true );
    return picked;
}

static skill_id skill_select_menu( const Character &c, const std::string &prompt )
{
    int i = 0;
    uilist nmenu;
    nmenu.text = prompt;
    for( const std::pair<const skill_id, SkillLevel> &s : *c._skills ) {
        bool enabled = s.second.level() > 0;
        std::string entry = string_format( "%s (%d)", s.first.str(), s.second.level() );
        nmenu.addentry( i, enabled, MENU_AUTOASSIGN, entry );
        i++;
    }
    nmenu.query();
    if( nmenu.ret > -1 ) {
        i = 0;
        for( const std::pair<const skill_id, SkillLevel> &s : *c._skills ) {
            if( i == nmenu.ret ) {
                return s.first;
            }
            i++;
        }
    }
    return skill_id();
}

static void npc_batch_override_toggle(
    const std::vector<npc *> &npc_list, ally_rule rule, bool state )
{
    for( npc *p : npc_list ) {
        p->rules.toggle_specific_override_state( rule, state );
    }
}

static void npc_temp_orders_menu( const std::vector<npc *> &npc_list )
{
    if( npc_list.empty() ) {
        return;
    }
    npc *guy = npc_list.front();
    Character &player_character = get_player_character();
    bool done = false;
    uilist nmenu;

    while( !done ) {
        int override_count = 0;
        std::string output_string = string_format( _( "%s currently has these temporary orders:" ),
                                    guy->get_name() );
        for( const auto &rule : ally_rule_strs ) {
            if( guy->rules.has_override_enable( rule.second.rule ) ) {
                override_count++;
                output_string += "\n  ";
                output_string += ( guy->rules.has_override( rule.second.rule ) ? rule.second.rule_true_text :
                                   rule.second.rule_false_text );
            }
        }
        if( override_count == 0 ) {
            output_string += std::string( "\n  " ) + _( "None." ) + "\n";
        }
        if( npc_list.size() > 1 ) {
            output_string += std::string( "\n" ) +
                             _( "Other followers might have different temporary orders." );
        }
        nmenu.reset();
        nmenu.text = _( "Issue what temporary order?" );
        nmenu.desc_enabled = true;
        parse_tags( output_string, player_character, *guy );
        nmenu.footer_text = output_string;
        nmenu.addentry( NPC_CHAT_DONE, true, 'd', _( "Done issuing orders" ) );
        nmenu.addentry( NPC_CHAT_FORBID_ENGAGE, true, 'f',
                        guy->rules.has_override_enable( ally_rule::forbid_engage ) ?
                        _( "Go back to your usual engagement habits" ) : _( "Don't engage hostiles for the time being" ) );
        nmenu.addentry( NPC_CHAT_NO_GUNS, true, 'g', guy->rules.has_override_enable( ally_rule::use_guns ) ?
                        _( "Use whatever weapon you normally would" ) : _( "Don't use ranged weapons for a while" ) );
        nmenu.addentry( NPC_CHAT_PULP, true, 'p', guy->rules.has_override_enable( ally_rule::allow_pulp ) ?
                        _( "Pulp zombies if you like" ) : _( "Hold off on pulping zombies for a while" ) );
        nmenu.addentry( NPC_CHAT_FOLLOW_CLOSE, true, 'c',
                        guy->rules.has_override_enable( ally_rule::follow_close ) &&
                        guy->rules.has_override( ally_rule::follow_close ) ?
                        _( "Go back to keeping your usual distance" ) : _( "Stick close to me for now" ) );
        nmenu.addentry( NPC_CHAT_MOVE_FREELY, true, 'm',
                        guy->rules.has_override_enable( ally_rule::follow_close ) &&
                        !guy->rules.has_override( ally_rule::follow_close ) ?
                        _( "Go back to keeping your usual distance" ) : _( "Move farther from me if you need to" ) );
        nmenu.addentry( NPC_CHAT_SLEEP, true, 's',
                        guy->rules.has_override_enable( ally_rule::allow_sleep ) ?
                        _( "Go back to your usual sleeping habits" ) : _( "Take a nap if you need it" ) );
        nmenu.addentry( NPC_CHAT_CLEAR_OVERRIDES, true, 'o', _( "Let's go back to your usual behaviors" ) );
        nmenu.query();

        switch( nmenu.ret ) {
            case NPC_CHAT_FORBID_ENGAGE:
                npc_batch_override_toggle( npc_list, ally_rule::forbid_engage, true );
                break;
            case NPC_CHAT_NO_GUNS:
                npc_batch_override_toggle( npc_list, ally_rule::use_guns, false );
                break;
            case NPC_CHAT_PULP:
                npc_batch_override_toggle( npc_list, ally_rule::allow_pulp, false );
                break;
            case NPC_CHAT_FOLLOW_CLOSE:
                npc_batch_override_toggle( npc_list, ally_rule::follow_close, true );
                break;
            case NPC_CHAT_MOVE_FREELY:
                npc_batch_override_toggle( npc_list, ally_rule::follow_close, false );
                break;
            case NPC_CHAT_SLEEP:
                npc_batch_override_toggle( npc_list, ally_rule::allow_sleep, true );
                break;
            case NPC_CHAT_CLEAR_OVERRIDES:
                for( npc *p : npc_list ) {
                    p->rules.clear_overrides();
                }
                break;
            default:
                done = true;
                break;
        }
    }

}

static void tell_veh_stop_following()
{
    Character &player_character = get_player_character();
    for( wrapped_vehicle &veh : get_map().get_vehicles() ) {
        vehicle *v = veh.v;
        if( v->has_engine_type( fuel_type_animal, false ) && v->is_owned_by( player_character ) ) {
            v->is_following = false;
            v->engine_on = false;
        }
    }
}

static void assign_veh_to_follow()
{
    Character &player_character = get_player_character();
    for( wrapped_vehicle &veh : get_map().get_vehicles() ) {
        vehicle *v = veh.v;
        if( v->has_engine_type( fuel_type_animal, false ) && v->is_owned_by( player_character ) ) {
            v->activate_animal_follow();
        }
    }
}

static void tell_magic_veh_to_follow()
{
    Character &player_character = get_player_character();
    for( wrapped_vehicle &veh : get_map().get_vehicles() ) {
        vehicle *v = veh.v;
        if( v->magic ) {
            for( const vpart_reference &vp : v->get_all_parts() ) {
                const vpart_info &vpi = vp.info();
                if( vpi.has_flag( "MAGIC_FOLLOW" ) && v->is_owned_by( player_character ) ) {
                    v->activate_magical_follow();
                    break;
                }
            }
        }
    }
}

static void tell_magic_veh_stop_following()
{
    for( wrapped_vehicle &veh : get_map().get_vehicles() ) {
        vehicle *v = veh.v;
        if( v->magic ) {
            for( const vpart_reference &vp : v->get_all_parts() ) {
                const vpart_info &vpi = vp.info();
                if( vpi.has_flag( "MAGIC_FOLLOW" ) ) {
                    v->is_following = false;
                    v->engine_on = false;
                    break;
                }
            }
        }
    }
}

void game::chat()
{
    Character &player_character = get_player_character();
    int volume = player_character.get_shout_volume();

    const std::vector<npc *> available = get_npcs_if( [&]( const npc & guy ) {
        // TODO: Get rid of the z-level check when z-level vision gets "better"
        return u.posz() == guy.posz() && u.sees( guy.pos() ) &&
               rl_dist( u.pos(), guy.pos() ) <= SEEX * 2;
    } );
    const int available_count = available.size();
    const std::vector<npc *> followers = get_npcs_if( [&]( const npc & guy ) {
        return guy.is_player_ally() && guy.is_following() && guy.can_hear( u.pos(), volume );
    } );
    const int follower_count = followers.size();
    const std::vector<npc *> guards = get_npcs_if( [&]( const npc & guy ) {
        return guy.mission == NPC_MISSION_GUARD_ALLY &&
               guy.companion_mission_role_id != "FACTION_CAMP" &&
               guy.can_hear( u.pos(), volume );
    } );
    const int guard_count = guards.size();

    if( player_character.has_trait( trait_PROF_FOODP ) &&
        !( player_character.is_wearing( itype_foodperson_mask ) ||
           player_character.is_wearing( itype_foodperson_mask_on ) ) ) {
        add_msg( m_warning, _( "You can't speak without your face!" ) );
        return;
    }
    std::vector<vehicle *> animal_vehicles;
    std::vector<vehicle *> following_vehicles;
    std::vector<vehicle *> magic_vehicles;
    std::vector<vehicle *> magic_following_vehicles;
    for( auto &veh : get_map().get_vehicles() ) {
        auto &v = veh.v;
        if( v->has_engine_type( fuel_type_animal, false ) &&
            v->is_owned_by( player_character ) ) {
            animal_vehicles.push_back( v );
            if( v->is_following ) {
                following_vehicles.push_back( v );
            }
        }
        if( v->magic ) {
            for( const vpart_reference &vp : v->get_all_parts() ) {
                const vpart_info &vpi = vp.info();
                if( vpi.has_flag( "MAGIC_FOLLOW" ) ) {
                    magic_vehicles.push_back( v );
                    if( v->is_following ) {
                        magic_following_vehicles.push_back( v );
                    }
                    break;
                }
            }
        }
    }

    uilist nmenu;
    nmenu.text = std::string( _( "What do you want to do?" ) );

    if( !available.empty() ) {
        const npc *guy = available.front();
        nmenu.addentry( NPC_CHAT_TALK, true, 't', available_count == 1 ?
                        string_format( _( "Talk to %s" ), guy->name_and_activity() ) :
                        _( "Talk to…" )
                      );
    }
    nmenu.addentry( NPC_CHAT_YELL, true, 'a', _( "Yell" ) );
    nmenu.addentry( NPC_CHAT_SENTENCE, true, 'b', _( "Yell a sentence" ) );
    if( !animal_vehicles.empty() ) {
        nmenu.addentry( NPC_CHAT_ANIMAL_VEHICLE_FOLLOW, true, 'F',
                        _( "Whistle at your animals pulling vehicles to follow you." ) );
    }
    if( !magic_vehicles.empty() ) {
        nmenu.addentry( NPC_CHAT_COMMAND_MAGIC_VEHICLE_FOLLOW, true, 'Q',
                        _( "Utter a magical command that will order your magical vehicles to follow you." ) );
    }
    if( !magic_following_vehicles.empty() ) {
        nmenu.addentry( NPC_CHAT_COMMAND_MAGIC_VEHICLE_STOP_FOLLOW, true, 'q',
                        _( "Utter a magical command that will order your magical vehicles to stop following you." ) );
    }
    if( !following_vehicles.empty() ) {
        nmenu.addentry( NPC_CHAT_ANIMAL_VEHICLE_STOP_FOLLOW, true, 'S',
                        _( "Whistle at your animals pulling vehicles to stop following you." ) );
    }
    if( !guards.empty() ) {
        nmenu.addentry( NPC_CHAT_FOLLOW, true, 'f', guard_count == 1 ?
                        string_format( _( "Tell %s to follow" ), guards.front()->get_name() ) :
                        _( "Tell someone to follow…" )
                      );
    }
    if( !followers.empty() ) {
        bool enable_seminar = !player_character.has_effect( effect_asked_to_train );
        nmenu.addentry( NPC_CHAT_START_SEMINAR, enable_seminar, 'T',
                        enable_seminar ? _( "Start a training seminar" ) :
                        _( "Start a training seminar (You've already taught enough for now)" ) );
        nmenu.addentry( NPC_CHAT_GUARD, true, 'g', follower_count == 1 ?
                        string_format( _( "Tell %s to guard" ), followers.front()->get_name() ) :
                        _( "Tell someone to guard…" )
                      );
        nmenu.addentry( NPC_CHAT_AWAKE, true, 'w', _( "Tell everyone on your team to wake up" ) );
        nmenu.addentry( NPC_CHAT_MOUNT, true, 'M', _( "Tell everyone on your team to mount up" ) );
        nmenu.addentry( NPC_CHAT_DISMOUNT, true, 'm', _( "Tell everyone on your team to dismount" ) );
        nmenu.addentry( NPC_CHAT_DANGER, true, 'D',
                        _( "Tell everyone on your team to prepare for danger" ) );
        nmenu.addentry( NPC_CHAT_CLEAR_OVERRIDES, true, 'r',
                        _( "Tell everyone on your team to relax (Clear Overrides)" ) );
        nmenu.addentry( NPC_CHAT_ORDERS, true, 'o', _( "Tell everyone on your team to temporarily…" ) );
    }
    std::string message;
    std::string yell_msg;
    bool is_order = true;
    nmenu.query();

    if( nmenu.ret < 0 ) {
        return;
    }

    switch( nmenu.ret ) {
        case NPC_CHAT_TALK: {
            const int npcselect = npc_select_menu( available, _( "Talk to whom?" ), false );
            if( npcselect < 0 ) {
                return;
            }
            get_avatar().talk_to( get_talker_for( *available[npcselect] ) );
            break;
        }
        case NPC_CHAT_YELL:
            is_order = false;
            message = _( "loudly." );
            break;
        case NPC_CHAT_SENTENCE: {
            std::string popupdesc = _( "Enter a sentence to yell" );
            string_input_popup popup;
            popup.title( _( "Yell a sentence" ) )
            .width( 64 )
            .description( popupdesc )
            .identifier( "sentence" )
            .max_length( 128 )
            .query();
            yell_msg = popup.text();
            is_order = false;
            break;
        }
        case NPC_CHAT_START_SEMINAR: {
            // TODO: Also allow group training of martial arts/spells/proficiencies
            const skill_id &sk = skill_select_menu( player_character,
                                                    _( "Which skill would you like to teach?" ) );
            if( !sk.is_valid() ) {
                return;
            }
            std::vector<int> selected = npcs_select_menu( followers,
            _( "Who should participate in the training seminar?" ), [&]( const npc * n ) {
                return !n || n->get_knowledge_level( sk ) >= player_character.get_skill_level( sk );
            } );
            if( selected.empty() ) {
                return;
            }
            std::vector<Character *> to_train;
            for( int i : selected ) {
                if( followers[i] ) {
                    to_train.push_back( followers[i] );
                }
            }
            talk_function::teach_domain d;
            d.skill = sk;
            d.style = matype_id();
            d.prof = proficiency_id();
            d.spell = spell_id();
            talk_function::start_training_gen( player_character, to_train, d );
            break;
        }
        case NPC_CHAT_GUARD: {
            const int npcselect = npc_select_menu( followers, _( "Who should guard here?" ) );
            if( npcselect < 0 ) {
                return;
            }
            if( npcselect == follower_count ) {
                for( npc *them : followers ) {
                    talk_function::assign_guard( *them );
                }
                yell_msg = _( "Everyone guard here!" );
            } else {
                talk_function::assign_guard( *followers[npcselect] );
                yell_msg = string_format( _( "Guard here, %s!" ), followers[npcselect]->get_name() );
            }
            break;
        }
        case NPC_CHAT_FOLLOW: {
            const int npcselect = npc_select_menu( guards, _( "Who should follow you?" ) );
            if( npcselect < 0 ) {
                return;
            }
            if( npcselect == guard_count ) {
                for( npc *them : guards ) {
                    talk_function::stop_guard( *them );
                }
                yell_msg = _( "Everyone follow me!" );
            } else {
                talk_function::stop_guard( *guards[npcselect] );
                yell_msg = string_format( _( "Follow me, %s!" ), guards[npcselect]->get_name() );
            }
            break;
        }
        case NPC_CHAT_AWAKE:
            for( npc *them : followers ) {
                talk_function::wake_up( *them );
            }
            yell_msg = _( "Stay awake!" );
            break;
        case NPC_CHAT_MOUNT:
            for( npc *them : followers ) {
                if( them->has_effect( effect_riding ) ) {
                    continue;
                }
                talk_function::find_mount( *them );
            }
            yell_msg = _( "Mount up!" );
            break;
        case NPC_CHAT_DISMOUNT:
            for( npc *them : followers ) {
                if( them->has_effect( effect_riding ) ) {
                    them->npc_dismount();
                }
            }
            yell_msg = _( "Dismount!" );
            break;
        case NPC_CHAT_DANGER:
            for( npc *them : followers ) {
                them->rules.set_danger_overrides();
            }
            yell_msg = _( "We're in danger.  Stay awake, stay close, don't go wandering off, "
                          "and don't open any doors." );
            break;
        case NPC_CHAT_CLEAR_OVERRIDES:
            for( npc *p : followers ) {
                talk_function::clear_overrides( *p );
            }
            yell_msg = _( "As you were." );
            break;
        case NPC_CHAT_ORDERS:
            npc_temp_orders_menu( followers );
            break;
        case NPC_CHAT_ANIMAL_VEHICLE_FOLLOW:
            assign_veh_to_follow();
            break;
        case NPC_CHAT_ANIMAL_VEHICLE_STOP_FOLLOW:
            tell_veh_stop_following();
            break;
        case NPC_CHAT_COMMAND_MAGIC_VEHICLE_FOLLOW:
            tell_magic_veh_to_follow();
            break;
        case NPC_CHAT_COMMAND_MAGIC_VEHICLE_STOP_FOLLOW:
            tell_magic_veh_stop_following();
            break;
        default:
            return;
    }

    if( !yell_msg.empty() ) {
        message = string_format( "\"%s\"", yell_msg );
    }
    if( !message.empty() ) {
        add_msg( _( "You yell %s" ), message );
        u.shout( string_format( _( "%s yelling %s" ), u.disp_name(), message ), is_order );
    }

    u.moves -= 100;
}

void npc::handle_sound( const sounds::sound_t spriority, const std::string &description,
                        int heard_volume, const tripoint &spos )
{
    const map &here = get_map();
    const tripoint s_abs_pos = here.getabs( spos );
    const tripoint my_abs_pos = here.getabs( pos() );

    add_msg_debug( debugmode::DF_NPC,
                   "%s heard '%s', priority %d at volume %d from %d:%d, my pos %d:%d",
                   disp_name(), description, static_cast<int>( spriority ), heard_volume,
                   s_abs_pos.x, s_abs_pos.y, my_abs_pos.x, my_abs_pos.y );

    Character &player_character = get_player_character();
    bool player_ally = player_character.pos() == spos && is_player_ally();
    Character *const sound_source = get_creature_tracker().creature_at<Character>( spos );
    bool npc_ally = sound_source && sound_source->is_npc() && is_ally( *sound_source );

    if( ( player_ally || npc_ally ) && spriority == sounds::sound_t::order ) {
        say( chatbin.snip_acknowledged );
    }

    if( sees( spos ) || is_hallucination() ) {
        return;
    }
    // ignore low priority sounds if the NPC "knows" it came from a friend.
    // TODO: NPC will need to respond to talking noise eventually
    // but only for bantering purposes, not for investigating.
    if( spriority < sounds::sound_t::alarm ) {
        if( player_ally ) {
            add_msg_debug( debugmode::DF_NPC, "Allied NPC ignored same faction %s", get_name() );
            return;
        }
        if( npc_ally ) {
            add_msg_debug( debugmode::DF_NPC, "NPC ignored same faction %s", get_name() );
            return;
        }
    }
    // discount if sound source is player, or seen by player,
    // and listener is friendly and sound source is combat or alert only.
    if( spriority < sounds::sound_t::alarm && player_character.sees( spos ) ) {
        if( is_player_ally() ) {
            add_msg_debug( debugmode::DF_NPC, "NPC %s ignored low priority noise that player can see",
                           get_name() );
            return;
            // discount if sound source is player, or seen by player,
            // listener is neutral and sound type is worth investigating.
        } else if( spriority < sounds::sound_t::destructive_activity &&
                   get_attitude_group( get_attitude() ) != attitude_group::hostile ) {
            return;
        }
    }
    // patrolling guards will investigate more readily than stationary NPCS
    int investigate_dist = 10;
    if( mission == NPC_MISSION_GUARD_ALLY || mission == NPC_MISSION_GUARD_PATROL ) {
        investigate_dist = 50;
    }
    if( rules.has_flag( ally_rule::ignore_noise ) ) {
        investigate_dist = 0;
    }
    if( ai_cache.total_danger < 1.0f ) {
        if( spriority == sounds::sound_t::movement && !in_vehicle ) {
            warn_about( "movement_noise", rng( 1, 10 ) * 1_minutes, description );
        } else if( spriority > sounds::sound_t::movement ) {
            if( ( spriority == sounds::sound_t::speech || spriority == sounds::sound_t::alert ||
                  spriority == sounds::sound_t::order ) && sound_source &&
                !has_faction_relationship( *sound_source, npc_factions::knows_your_voice ) ) {
                warn_about( "speech_noise", rng( 1, 10 ) * 1_minutes );
            } else if( spriority > sounds::sound_t::activity ) {
                warn_about( "combat_noise", rng( 1, 10 ) * 1_minutes );
            }
            bool should_check = rl_dist( pos(), spos ) < investigate_dist;
            if( should_check ) {
                const zone_manager &mgr = zone_manager::get_manager();
                // NOLINTNEXTLINE(bugprone-branch-clone)
                if( mgr.has( zone_type_NPC_NO_INVESTIGATE, s_abs_pos, fac_id ) ) {
                    should_check = false;
                } else if( mgr.has( zone_type_NPC_INVESTIGATE_ONLY, my_abs_pos, fac_id ) &&
                           !mgr.has( zone_type_NPC_INVESTIGATE_ONLY, s_abs_pos, fac_id ) ) {
                    should_check = false;
                }
            }
            if( should_check ) {
                add_msg_debug( debugmode::DF_NPC, "%s added noise at pos %d:%d", get_name(), s_abs_pos.x,
                               s_abs_pos.y );
                dangerous_sound temp_sound;
                temp_sound.abs_pos = s_abs_pos;
                temp_sound.volume = heard_volume;
                temp_sound.type = spriority;
                if( !ai_cache.sound_alerts.empty() ) {
                    if( ai_cache.sound_alerts.back().abs_pos != s_abs_pos ) {
                        ai_cache.sound_alerts.push_back( temp_sound );
                    }
                } else {
                    ai_cache.sound_alerts.push_back( temp_sound );
                }
            }
        }
    }
}

void avatar::talk_to( std::unique_ptr<talker> talk_with, bool radio_contact,
                      bool is_computer )
{
    const bool has_mind_control = has_trait( trait_DEBUG_MIND_CONTROL );
    if( !talk_with->will_talk_to_u( *this, has_mind_control ) ) {
        return;
    }
    dialogue d( get_talker_for( *this ), std::move( talk_with ) );
    d.by_radio = radio_contact;
    dialogue_by_radio = radio_contact;
    d.actor( true )->check_missions();
    for( auto &mission : d.actor( true )->assigned_missions() ) {
        if( mission->get_assigned_player_id() == getID() ) {
            d.missions_assigned.push_back( mission );
        }
    }
    for( const std::string &topic_id : d.actor( true )->get_topics( radio_contact ) ) {
        d.add_topic( topic_id );
    }
    for( const std::string &topic_id : d.actor( true )->get_topics( radio_contact ) ) {
        d.add_topic( topic_id );
    }
    dialogue_window d_win;
    d_win.is_computer = is_computer;
    // Main dialogue loop
    do {
        d.actor( true )->update_missions( d.missions_assigned );
        const talk_topic next = d.opt( d_win, d.topic_stack.back() );
        if( next.id == "TALK_NONE" ) {
            int cat = topic_category( d.topic_stack.back() );
            do {
                d.topic_stack.pop_back();
            } while( cat != -1 && topic_category( d.topic_stack.back() ) == cat );
        }
        if( next.id == "TALK_DONE" || d.topic_stack.empty() ) {
            npc *npc_actor = d.actor( true )->get_npc();
            if( npc_actor ) {
                d.actor( true )->say( _( npc_actor->chatbin.snip_bye ) );
            }
            d.done = true;
        } else if( next.id != "TALK_NONE" ) {
            d.add_topic( next );
        }
    } while( !d.done );

    if( activity.id() == ACT_AIM && !has_weapon() ) {
        cancel_activity();
        // don't query certain activities that are started from dialogue
    } else if( activity.id() == ACT_TRAIN || activity.id() == ACT_WAIT_NPC ||
               activity.id() == ACT_SOCIALIZE || activity.index == d.actor( true )->getID().get_value() ) {
        return;
    }

    if( !d.actor( true )->has_effect( effect_under_operation, bodypart_str_id::NULL_ID() ) ) {
        g->cancel_activity_or_ignore_query( distraction_type::talked_to,
                                            string_format( _( "%s talked to you." ),
                                                    d.actor( true )->disp_name() ) );
    }
}

std::string dialogue::dynamic_line( const talk_topic &the_topic ) const
{
    if( !the_topic.item_type.is_null() ) {
        cur_item = the_topic.item_type;
    }

    // For compatibility
    const auto &topic = the_topic.id;
    const auto iter = json_talk_topics.find( topic );
    if( iter != json_talk_topics.end() ) {
        const std::string line = iter->second.get_dynamic_line( *this );
        if( !line.empty() ) {
            return line;
        }
    }

    if( topic == "TALK_NPC_NOFACE" ) {
        return string_format( _( "&%s stays silent." ), actor( true )->disp_name() );
    }

    if( topic == "TALK_NOFACE" ) {
        return _( "&You can't talk without your face." );
    } else if( topic == "TALK_DEAF" ) {
        return _( "&You are deaf and can't talk." );

    } else if( topic == "TALK_DEAF_ANGRY" ) {
        return string_format(
                   _( "&You are deaf and can't talk.  When you don't respond, %s becomes angry!" ),
                   actor( true )->disp_name() );
    } else if( topic == "TALK_MUTE" ) {
        return _( "&You are mute and can't talk." );

    } else if( topic == "TALK_MUTE_ANGRY" ) {
        return string_format(
                   _( "&You are mute and can't talk.  When you don't respond, %s becomes angry!" ),
                   actor( true )->disp_name() );
    }
    avatar &player_character = get_avatar();
    if( topic == "TALK_SEDATED" ) {
        return string_format( _( "%1$s is sedated and can't be moved or woken up until the "
                                 "medication or sedation wears off.\nYou estimate it will wear "
                                 "off in %2$s." ),
                              actor( true )->disp_name(),
                              to_string_approx( player_character.estimate_effect_dur( skill_firstaid,
                                                effect_narcosis, 90_minutes, 60_minutes, 6,
                                                *actor( true )->get_npc() ) ) );
    }

    // Those topics are handled by the mission system, see there.
    static const std::unordered_set<std::string> mission_topics = { {
            "TALK_MISSION_DESCRIBE", "TALK_MISSION_DESCRIBE_URGENT",
            "TALK_MISSION_OFFER", "TALK_MISSION_ACCEPTED",
            "TALK_MISSION_REJECTED", "TALK_MISSION_ADVICE", "TALK_MISSION_INQUIRE",
            "TALK_MISSION_SUCCESS", "TALK_MISSION_SUCCESS_LIE", "TALK_MISSION_FAILURE"
        }
    };
    if( mission_topics.count( topic ) > 0 ) {
        mission *miss = actor( true )->selected_mission();
        if( miss == nullptr ) {
            return "mission_selected == nullptr; BUG!  (npctalk.cpp:dynamic_line)";
        }
        const auto &type = miss->get_type();
        // TODO: make it a member of the mission class, maybe at mission instance specific data
        const std::string &ret = miss->dialogue_for_topic( topic );
        if( ret.empty() ) {
            debugmsg( "Bug in npctalk.cpp:dynamic_line.  Wrong mission_id(%s) or topic(%s)",
                      type.id.c_str(), topic.c_str() );
            return "";
        }

        if( topic == "TALK_MISSION_SUCCESS" && miss->has_follow_up() ) {
            switch( rng( 1, 3 ) ) {
                case 1:
                    return ret + _( "  And I have more I'd like you to do." );
                case 2:
                    return ret + _( "  I could use a hand with something else if you are interested." );
                case 3:
                    return ret + _( "  If you are interested, I have another job for you." );
            }
        }

        return ret;
    }

    if( topic == "TALK_NONE" || topic == "TALK_DONE" ) {
        return _( actor( true )->get_npc()->chatbin.snip_bye );
    } else if( topic == "TALK_TRAIN" ) {
        if( !player_character.backlog.empty() && player_character.backlog.front().id() == ACT_TRAIN ) {
            return _( "Shall we resume?" );
        } else if( actor( true )->skills_offered_to( *actor( false ) ).empty() &&
                   actor( true )->styles_offered_to( *actor( false ) ).empty() &&
                   actor( true )->spells_offered_to( *actor( false ) ).empty() ) {
            return _( "Sorry, but it doesn't seem I have anything to teach you." );
        } else {
            return _( "Here's what I can teach you…" );
        }
    } else if( topic == "TALK_TRAIN_NPC" ) {
        if( actor( false )->skills_offered_to( *actor( true ) ).empty() ) {
            return _( "Sorry, there's nothing I can learn from you." );
        } else {
            return _( "Sure, I'm all ears." );
        }
    } else if( topic == "TALK_TRAIN_SEMINAR" ) {
        return _( "What do you want me to teach?" );
    } else if( topic == "TALK_HOW_MUCH_FURTHER" ) {
        return actor( true )->distance_to_goal();
    } else if( topic == "TALK_DESCRIBE_MISSION" ) {
        return actor( true )->get_job_description();
    } else if( topic == "TALK_SHOUT" ) {
        actor( false )->shout();
        if( actor( false )->is_deaf() ) {
            return _( "&You yell, but can't hear yourself." );
        } else {
            if( actor( false )->is_mute() ) {
                return _( "&You yell, but can't form words." );
            } else {
                return _( "&You yell." );
            }
        }
    } else if( topic == "TALK_SIZE_UP" ) {
        return actor( true )->evaluation_by( *actor( false ) );
    } else if( topic == "TALK_LOOK_AT" ) {
        return "&" + actor( true )->short_description();
    } else if( topic == "TALK_OPINION" ) {
        return "&" + actor( true )->opinion_text();
    } else if( topic == "TALK_MIND_CONTROL" ) {
        if( actor( true )->enslave_mind() ) {
            return _( "YES, MASTER!" );
        }
    }

    debugmsg( "I don't know what to say for %s. (BUG (npctalk.cpp:dynamic_line))", topic );
    return "";
}

void dialogue::apply_speaker_effects( const talk_topic &the_topic )
{
    const auto &topic = the_topic.id;
    const auto iter = json_talk_topics.find( topic );
    if( iter == json_talk_topics.end() ) {
        return;
    }
    for( json_dynamic_line_effect &npc_effect : iter->second.get_speaker_effects() ) {
        if( npc_effect.test_condition( *this ) ) {
            npc_effect.apply( *this );
        }
    }
}

talk_response &dialogue::add_response( const std::string &text, const std::string &r,
                                       const bool first )
{
    talk_response result = talk_response();
    result.truetext = no_translation( text );
    result.truefalse_condition = []( const dialogue & ) {
        return true;
    };
    result.success.next_topic = talk_topic( r );
    if( first ) {
        responses.insert( responses.begin(), result );
        return responses.front();
    } else {
        responses.push_back( result );
        return responses.back();
    }
}

talk_response &dialogue::add_response_done( const std::string &text )
{
    return add_response( text, "TALK_DONE" );
}

talk_response &dialogue::add_response_none( const std::string &text )
{
    return add_response( text, "TALK_NONE" );
}

talk_response &dialogue::add_response( const std::string &text, const std::string &r,
                                       const talkfunction_ptr &effect_success, const bool first )
{
    talk_response &result = add_response( text, r, first );
    result.success.set_effect( effect_success );
    return result;
}

talk_response &dialogue::add_response( const std::string &text, const std::string &r,
                                       const std::function<void( npc & )> &effect_success,
                                       dialogue_consequence consequence, const bool first )
{
    talk_response &result = add_response( text, r, first );
    result.success.set_effect_consequence( effect_success, consequence );
    return result;
}

talk_response &dialogue::add_response( const std::string &text, const std::string &r,
                                       mission *miss, const bool first )
{
    if( miss == nullptr ) {
        debugmsg( "tried to select null mission" );
    }
    talk_response &result = add_response( text, r, first );
    result.mission_selected = miss;
    return result;
}

talk_response &dialogue::add_response( const std::string &text, const std::string &r,
                                       const skill_id &skill, const bool first )
{
    talk_response &result = add_response( text, r, first );
    result.skill = skill;
    return result;
}

talk_response &dialogue::add_response( const std::string &text, const std::string &r,
                                       const proficiency_id &proficiency, const bool first )
{
    talk_response &result = add_response( text, r, first );
    result.proficiency = proficiency;
    return result;
}

talk_response &dialogue::add_response( const std::string &text, const std::string &r,
                                       const spell_id &sp, const bool first )
{
    talk_response &result = add_response( text, r, first );
    result.dialogue_spell = sp;
    return result;
}

talk_response &dialogue::add_response( const std::string &text, const std::string &r,
                                       const martialart &style, const bool first )
{
    talk_response &result = add_response( text, r, first );
    result.style = style.id;
    return result;
}

talk_response &dialogue::add_response( const std::string &text, const std::string &r,
                                       const itype_id &item_type, const bool first )
{
    if( item_type.is_null() ) {
        debugmsg( "explicitly specified null item" );
    }

    talk_response &result = add_response( text, r, first );
    result.success.next_topic.item_type = item_type;
    return result;
}

void dialogue::gen_responses( const talk_topic &the_topic )
{
    responses.clear();
    const auto iter = json_talk_topics.find( the_topic.id );
    if( iter != json_talk_topics.end() ) {
        json_talk_topic &jtt = iter->second;
        if( jtt.gen_responses( *this ) ) {
            return;
        }
    }

    Character &player_character = get_player_character();
    if( the_topic.id == "TALK_MISSION_LIST" ) {
        if( actor( true )->available_missions().size() == 1 ) {
            add_response( _( "Tell me about it." ), "TALK_MISSION_OFFER",
                          actor( true )->available_missions().front(), true );
        } else {
            for( auto &mission : actor( true )->available_missions() ) {
                add_response( mission->get_type().tname(), "TALK_MISSION_OFFER", mission, true );
            }
        }
    } else if( the_topic.id == "TALK_MISSION_LIST_ASSIGNED" ) {
        if( missions_assigned.size() == 1 ) {
            add_response( _( "I have news." ), "TALK_MISSION_INQUIRE", missions_assigned.front() );
        } else {
            for( auto &miss_it : missions_assigned ) {
                add_response( miss_it->get_type().tname(), "TALK_MISSION_INQUIRE", miss_it );
            }
        }
    } else if( the_topic.id == "TALK_TRAIN_NPC" ) {
        const std::vector<skill_id> &trainable = actor( false )->skills_offered_to( *actor( true ) );
        if( trainable.empty() ) {
            add_response_none( _( "Oh, okay." ) );
            return;
        }
        for( const skill_id &s : trainable ) {
            const std::string &text = actor( true )->skill_training_text( *actor( true ), s );
            if( !text.empty() && !s->obsolete() ) {
                add_response( text, "TALK_TRAIN_NPC_START", s );
            }
        }
        add_response_none( _( "Eh, never mind." ) );
    } else if( the_topic.id == "TALK_TRAIN_SEMINAR" ) {
        const std::vector<skill_id> &slist = actor( true )->skills_teacheable();
        if( slist.empty() ) {
            add_response_none( _( "Oh, okay." ) );
            return;
        }
        for( const skill_id &sk : slist ) {
            if( sk->obsolete() ) {
                continue;
            }
            const std::string &text = actor( true )->skill_seminar_text( sk );
            add_response( text, "TALK_TRAIN_SEMINAR_START", sk );
        }
        add_response_none( _( "Eh, never mind." ) );
    } else if( the_topic.id == "TALK_TRAIN" ) {
        if( !player_character.backlog.empty() && player_character.backlog.front().id() == ACT_TRAIN &&
            player_character.backlog.front().index == actor( true )->getID().get_value() ) {
            player_activity &backlog = player_character.backlog.front();
            const skill_id skillt( backlog.name );
            // TODO: This is potentially dangerous. A skill and a martial art
            // could have the same ident!
            if( !skillt.is_valid() ) {
                const matype_id styleid = matype_id( backlog.name );
                if( !styleid.is_valid() ) {
                    const spell_id &sp_id = spell_id( backlog.name );
                    if( actor( true )->knows_spell( sp_id ) ) {
                        add_response( string_format( _( "Yes, let's resume training %s" ),
                                                     sp_id->name ), "TALK_TRAIN_START", sp_id );
                    }
                } else {
                    const martialart &style = styleid.obj();
                    add_response( string_format( _( "Yes, let's resume training %s" ),
                                                 style.name ), "TALK_TRAIN_START", style );
                }
            } else {
                add_response( string_format( _( "Yes, let's resume training %s" ), skillt->name() ),
                              "TALK_TRAIN_START", skillt );
            }
        }
        const std::vector<matype_id> &styles = actor( true )->styles_offered_to( *actor( false ) );
        const std::vector<skill_id> &trainable = actor( true )->skills_offered_to( *actor( false ) );
        const std::vector<spell_id> &teachable = actor( true )->spells_offered_to( *actor( false ) );
        const std::vector<proficiency_id> &proficiencies = actor( true )->proficiencies_offered_to( *actor(
                    false ) );
        if( trainable.empty() && styles.empty() && teachable.empty() && proficiencies.empty() ) {
            add_response_none( _( "Oh, okay." ) );
            return;
        }
        for( const spell_id &sp : teachable ) {
            const std::string &text = actor( true )->spell_training_text( *actor( false ), sp );
            if( !text.empty() ) {
                add_response( text, "TALK_TRAIN_START", sp );
            }
        }
        for( const matype_id &style_id : styles ) {
            const std::string &text = actor( true )->style_training_text( *actor( false ), style_id );
            if( !text.empty() ) {
                add_response( text, "TALK_TRAIN_START", style_id.obj() );
            }
        }
        for( const skill_id &trained : trainable ) {
            const std::string &text = actor( true )->skill_training_text( *actor( false ), trained );
            if( !text.empty() && !trained->obsolete() ) {
                add_response( text, "TALK_TRAIN_START", trained );
            }
        }
        for( const proficiency_id &trained : proficiencies ) {
            const std::string &text = actor( true )->proficiency_training_text( *actor( false ), trained );
            if( !text.empty() ) {
                add_response( text, "TALK_TRAIN_START", trained );
            }
        }
        add_response_none( _( "Eh, never mind." ) );
    } else if( the_topic.id == "TALK_HOW_MUCH_FURTHER" ) {
        add_response_none( _( "Okay, thanks." ) );
        add_response_done( _( "Let's keep moving." ) );
    }

    if( actor( false )->has_trait( trait_DEBUG_MIND_CONTROL ) && !actor( true )->is_player_ally() ) {
        add_response( _( "OBEY ME!" ), "TALK_MIND_CONTROL" );
        add_response_done( _( "Bye." ) );
    }

    if( responses.empty() ) {
        add_response_done( _( "Bye." ) );
    }
}

static int parse_mod( const dialogue &d, const std::string &attribute, const int factor )
{
    return d.actor( true )->parse_mod( attribute, factor ) + d.actor( false )->parse_mod( attribute,
            factor );
}

int talk_trial::calc_chance( const dialogue &d ) const
{
    if( d.actor( false )->has_trait( trait_DEBUG_MIND_CONTROL ) ) {
        return 100;
    }
    int chance = difficulty;
    switch( type ) {
        case NUM_TALK_TRIALS:
            dbg( D_ERROR ) << "called calc_chance with invalid talk_trial value: " << type;
            break;
        case TALK_TRIAL_NONE:
            chance = 100;
            break;
        case TALK_TRIAL_CONDITION:
            chance = condition( d ) ? 100 : 0;
            break;
        case TALK_TRIAL_LIE:
            chance += d.actor( false )->trial_chance_mod( "lie" ) + d.actor( true )->trial_chance_mod( "lie" );
            break;
        case TALK_TRIAL_PERSUADE:
            chance += d.actor( false )->trial_chance_mod( "persuade" ) +
                      d.actor( true )->trial_chance_mod( "persuade" );
            break;
        case TALK_TRIAL_INTIMIDATE:
            chance += d.actor( false )->trial_chance_mod( "intimidate" ) +
                      d.actor( true )->trial_chance_mod( "intimidate" );
            break;
    }
    for( const auto &this_mod : modifiers ) {
        chance += parse_mod( d, this_mod.first, this_mod.second );
    }

    return std::max( 0, std::min( 100, chance ) );
}

bool talk_trial::roll( dialogue &d ) const
{
    if( type == TALK_TRIAL_NONE || d.actor( false )->has_trait( trait_DEBUG_MIND_CONTROL ) ) {
        return true;
    }
    const int chance = calc_chance( d );
    const bool success = rng( 0, 99 ) < chance;
    const bool speech_trial = type == TALK_TRIAL_PERSUADE || type == TALK_TRIAL_INTIMIDATE ||
                              type == TALK_TRIAL_LIE;
    if( speech_trial && d.actor( false )->get_character() ) {
        Character &u = *d.actor( false )->get_character();
        if( success ) {
            u.practice( skill_speech, ( 100 - chance ) / 10 );
        } else {
            u.practice( skill_speech, ( 100 - chance ) / 7 );
        }
    }
    return success;
}

int topic_category( const talk_topic &the_topic )
{
    const auto &topic = the_topic.id;
    // TODO: ideally, this would be a property of the topic itself.
    // How this works: each category has a set of topics that belong to it, each set is checked
    // for the given topic and if a set contains, the category number is returned.
    static const std::unordered_set<std::string> topic_1 = { {
            "TALK_MISSION_START", "TALK_MISSION_DESCRIBE", "TALK_MISSION_OFFER",
            "TALK_MISSION_ACCEPTED", "TALK_MISSION_REJECTED", "TALK_MISSION_ADVICE",
            "TALK_MISSION_INQUIRE", "TALK_MISSION_SUCCESS", "TALK_MISSION_SUCCESS_LIE",
            "TALK_MISSION_FAILURE", "TALK_MISSION_REWARD", "TALK_MISSION_END",
            "TALK_MISSION_DESCRIBE_URGENT"
        }
    };
    if( topic_1.count( topic ) > 0 ) {
        return 1;
    }
    static const std::unordered_set<std::string> topic_2 = { {
            "TALK_SHARE_EQUIPMENT", "TALK_GIVE_EQUIPMENT", "TALK_DENY_EQUIPMENT"
        }
    };
    if( topic_2.count( topic ) > 0 ) {
        return 2;
    }
    static const std::unordered_set<std::string> topic_3 = { {
            "TALK_SUGGEST_FOLLOW", "TALK_AGREE_FOLLOW", "TALK_DENY_FOLLOW",
        }
    };
    if( topic_3.count( topic ) > 0 ) {
        return 3;
    }
    static const std::unordered_set<std::string> topic_4 = { {
            "TALK_COMBAT_ENGAGEMENT",
        }
    };
    if( topic_4.count( topic ) > 0 ) {
        return 4;
    }
    static const std::unordered_set<std::string> topic_5 = { {
            "TALK_COMBAT_COMMANDS",
        }
    };
    if( topic_5.count( topic ) > 0 ) {
        return 5;
    }
    static const std::unordered_set<std::string> topic_6 = { {
            "TALK_TRAIN", "TALK_TRAIN_START", "TALK_TRAIN_FORCE",
            "TALK_TRAIN_NPC_START", "TALK_TRAIN_NPC_FORCE"
        }
    };
    if( topic_6.count( topic ) > 0 ) {
        return 6;
    }
    static const std::unordered_set<std::string> topic_7 = { {
            "TALK_MISC_RULES",
        }
    };
    if( topic_7.count( topic ) > 0 ) {
        return 7;
    }
    static const std::unordered_set<std::string> topic_8 = { {
            "TALK_AIM_RULES",
        }
    };
    if( topic_8.count( topic ) > 0 ) {
        return 8;
    }
    static const std::unordered_set<std::string> topic_9 = { {
            "TALK_FRIEND", "TALK_GIVE_ITEM", "TALK_USE_ITEM",
        }
    };
    if( topic_9.count( topic ) > 0 ) {
        return 9;
    }
    static const std::unordered_set<std::string> topic_99 = { {
            "TALK_SIZE_UP", "TALK_LOOK_AT", "TALK_OPINION", "TALK_SHOUT"
        }
    };
    if( topic_99.count( topic ) > 0 ) {
        return 99;
    }
    return -1; // Not grouped with other topics
}

void parse_tags( std::string &phrase, const Character &u, const Character &me,
                 const itype_id &item_type )
{
    phrase = SNIPPET.expand( remove_color_tags( phrase ) );

    size_t fa;
    size_t fb;
    std::string tag;
    do {
        fa = phrase.find( '<' );
        fb = phrase.find( '>' );
        int l = fb - fa + 1;
        if( fa != std::string::npos && fb != std::string::npos ) {
            tag = phrase.substr( fa, fb - fa + 1 );
        } else {
            return;
        }

        const item &u_weapon = u.get_wielded_item();
        const item &me_weapon = me.get_wielded_item();
        // Special, dynamic tags go here
        if( tag == "<yrwp>" ) {
            phrase.replace( fa, l, remove_color_tags( u_weapon.tname() ) );
        } else if( tag == "<mywp>" ) {
            if( !me.is_armed() ) {
                phrase.replace( fa, l, _( "fists" ) );
            } else {
                phrase.replace( fa, l, remove_color_tags( me_weapon.tname() ) );
            }
        } else if( tag == "<ammo>" ) {
            if( !me_weapon.is_gun() ) {
                phrase.replace( fa, l, _( "BADAMMO" ) );
            } else {
                phrase.replace( fa, l, me_weapon.ammo_current()->nname( 1 ) );
            }
        } else if( tag == "<current_activity>" ) {
            std::string activity_name;
            const npc *guy = dynamic_cast<const npc *>( &me );
            if( guy->current_activity_id ) {
                activity_name = guy->get_current_activity();
            } else {
                activity_name = _( "doing this and that" );
            }
            phrase.replace( fa, l, activity_name );
        } else if( tag == "<punc>" ) {
            switch( rng( 0, 2 ) ) {
                case 0:
                    phrase.replace( fa, l, pgettext( "punctuation", "." ) );
                    break;
                case 1:
                    phrase.replace( fa, l, pgettext( "punctuation", "…" ) );
                    break;
                case 2:
                    phrase.replace( fa, l, pgettext( "punctuation", "!" ) );
                    break;
            }
        } else if( tag == "<mypronoun>" ) {
            std::string npcstr = me.male ? pgettext( "npc", "He" ) : pgettext( "npc", "She" );
            phrase.replace( fa, l, npcstr );
        } else if( tag == "<topic_item>" ) {
            phrase.replace( fa, l, item::nname( item_type, 2 ) );
        } else if( tag == "<topic_item_price>" ) {
            item tmp( item_type );
            phrase.replace( fa, l, format_money( tmp.price( true ) ) );
        } else if( tag == "<topic_item_my_total_price>" ) {
            item tmp( item_type );
            tmp.charges = me.charges_of( item_type );
            phrase.replace( fa, l, format_money( tmp.price( true ) ) );
        } else if( tag == "<topic_item_your_total_price>" ) {
            item tmp( item_type );
            tmp.charges = u.charges_of( item_type );
            phrase.replace( fa, l, format_money( tmp.price( true ) ) );
        } else if( !tag.empty() ) {
            debugmsg( "Bad tag.  '%s' (%d - %d)", tag.c_str(), fa, fb );
            phrase.replace( fa, fb - fa + 1, "????" );
        }
    } while( fa != std::string::npos && fb != std::string::npos );
}

void dialogue::add_topic( const std::string &topic_id )
{
    topic_stack.emplace_back( topic_id );
}

void dialogue::add_topic( const talk_topic &topic )
{
    topic_stack.push_back( topic );
}

talker *dialogue::actor( const bool is_beta ) const
{
    if( !has_beta && !has_alpha ) {
        debugmsg( "Attempted to use a dialogue with no actors!" );
    }
    if( is_beta && !has_beta ) {
        debugmsg( "Tried to use an invalid beta talker." );
        // Try to avoid a crash by using the alpha if it exists
        if( has_alpha ) {
            return alpha.get();
        }
    }
    if( !is_beta && !has_alpha ) {
        debugmsg( "Tried to use an invalid alpha talker." );
        // Try to avoid a crash by using the beta if it exists
        if( has_beta ) {
            return beta.get();
        }
    }
    return ( is_beta ? beta : alpha ).get();
}

dialogue::dialogue( std::unique_ptr<talker> alpha_in,
                    std::unique_ptr<talker> beta_in )
{
    has_alpha = alpha_in != nullptr;
    has_beta = beta_in != nullptr;
    if( has_alpha ) {
        alpha = std::move( alpha_in );
    }
    if( has_beta ) {
        beta = std::move( beta_in );
    }
    if( !has_alpha && !has_beta ) {
        debugmsg( "Constructed a dialogue with no actors!" );
    }
}

talk_data talk_response::create_option_line( const dialogue &d, const input_event &hotkey )
{
    std::string ftext;
    text = ( truefalse_condition( d ) ? truetext : falsetext ).translated();
    if( trial.type == TALK_TRIAL_NONE || trial.type == TALK_TRIAL_CONDITION ) {
        // regular dialogue
        ftext = text;
    } else {
        // dialogue w/ a % chance to work
        //~ %1$s is translated trial type, %2$d is a number, and %3$s is the translated response text
        ftext = string_format( pgettext( "talk option", "[%1$s %2$d%%] %3$s" ),
                               trial.name(), trial.calc_chance( d ), text );
    }
    parse_tags( ftext, *d.actor( false )->get_character(), *d.actor( true )->get_npc(),
                success.next_topic.item_type );

    nc_color color;
    std::set<dialogue_consequence> consequences = get_consequences( d );
    if( consequences.count( dialogue_consequence::hostile ) > 0 ) {
        color = c_red;
    } else if( text[0] == '*' || consequences.count( dialogue_consequence::helpless ) > 0 ) {
        color = c_light_red;
    } else if( text[0] == '&' || consequences.count( dialogue_consequence::action ) > 0 ) {
        color = c_green;
    } else {
        color = c_white;
    }
    talk_data results;
    results.color = color;
    results.hotkey_desc = right_justify( hotkey.short_description(), 2 );
    results.text = ftext;
    return results;
}

std::set<dialogue_consequence> talk_response::get_consequences( const dialogue &d ) const
{
    int chance = trial.calc_chance( d );
    if( chance >= 100 ) {
        return { success.get_consequence( d ) };
    } else if( chance <= 0 ) {
        return { failure.get_consequence( d ) };
    }

    return {{ success.get_consequence( d ), failure.get_consequence( d ) }};
}

dialogue_consequence talk_effect_t::get_consequence( const dialogue &d ) const
{
    if( d.actor( true )->check_hostile_response( opinion.anger ) ) {
        return dialogue_consequence::hostile;
    }
    return guaranteed_consequence;
}

const talk_topic &special_talk( const std::string &action )
{
    static const std::map<std::string, talk_topic> key_map = {{
            { "LOOK_AT", talk_topic( "TALK_LOOK_AT" ) },
            { "SIZE_UP_STATS", talk_topic( "TALK_SIZE_UP" ) },
            { "CHECK_OPINION", talk_topic( "TALK_OPINION" ) },
            { "YELL", talk_topic( "TALK_SHOUT" ) },
        }
    };

    const auto iter = key_map.find( action );
    if( iter != key_map.end() ) {
        return iter->second;
    }

    static const talk_topic no_topic = talk_topic( "TALK_NONE" );
    return no_topic;
}

talk_topic dialogue::opt( dialogue_window &d_win, const talk_topic &topic )
{
    d_win.add_history_separator();

    ui_adaptor ui;
    const auto resize_cb = [&]( ui_adaptor & ui ) {
        d_win.resize( ui );
    };
    ui.on_screen_resize( resize_cb );
    resize_cb( ui );

    // Construct full line
    std::string challenge = dynamic_line( topic );
    gen_responses( topic );
    // Put quotes around challenge (unless it's an action)
    if( challenge[0] != '*' && challenge[0] != '&' ) {
        challenge = "\"" + challenge + "\"";
    }

    // Parse any tags in challenge
    parse_tags( challenge, *actor( false )->get_character(), *actor( true )->get_npc(),
                topic.item_type );
    capitalize_letter( challenge );

    d_win.clear_history_highlights();
    if( challenge[0] == '&' ) {
        // No name prepended!
        challenge = challenge.substr( 1 );
        d_win.add_to_history( challenge );
    } else if( challenge[0] == '*' ) {
        // Prepend name
        challenge = string_format( pgettext( "npc does something", "%s %s" ), actor( true )->disp_name(),
                                   challenge.substr( 1 ) );
        d_win.add_to_history( challenge );
    } else {
        npc *npc_actor = actor( true )->get_npc();
        d_win.add_to_history( challenge, actor( true )->disp_name(),
                              npc_actor ? npc_actor->basic_symbol_color() : c_red );
    }

    apply_speaker_effects( topic );

    if( responses.empty() ) {
        debugmsg( "No dialogue responses" );
        return talk_topic( "TALK_NONE" );
    }

    input_context ctxt( "DIALOGUE_CHOOSE_RESPONSE" );
    if( !d_win.is_computer ) {
        ctxt.register_action( "LOOK_AT" );
        ctxt.register_action( "SIZE_UP_STATS" );
        ctxt.register_action( "YELL" );
        ctxt.register_action( "CHECK_OPINION" );
    }
    ctxt.register_updown();
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "PAGE_DOWN" );
    ctxt.register_action( "HELP_KEYBINDINGS" );
    ctxt.register_action( "ANY_INPUT" );
    ctxt.register_action( "QUIT" );
    std::vector<talk_data> response_lines;
    std::vector<input_event> response_hotkeys;
    const auto generate_response_lines = [&]() {
#if defined(__ANDROID__)
        ctxt.get_registered_manual_keys().clear();
#endif
        const hotkey_queue &queue = hotkey_queue::alphabets();
        response_lines.clear();
        response_hotkeys.clear();
        input_event evt = ctxt.first_unassigned_hotkey( queue );
        for( talk_response &response : responses ) {
            const talk_data &td = response.create_option_line( *this, evt );
            response_lines.emplace_back( td );
            response_hotkeys.emplace_back( evt );
#if defined(__ANDROID__)
            ctxt.register_manual_key( evt.get_first_input(), td.text );
#endif
            evt = ctxt.next_unassigned_hotkey( queue, evt );
        }
    };
    generate_response_lines();

    ui.on_redraw( [&]( const ui_adaptor & ) {
        d_win.draw( actor( true )->disp_name(), response_lines );
    } );

    size_t response_ind = response_hotkeys.size();
    bool okay;
    do {
        d_win.refresh_response_display();
        std::string action;
        do {
            ui_manager::redraw();
            input_event evt;
            action = ctxt.handle_input();
            evt = ctxt.get_raw_input();
            d_win.handle_scrolling( action );
            talk_topic st = special_talk( action );
            if( st.id != "TALK_NONE" ) {
                return st;
            }
            if( action == "HELP_KEYBINDINGS" ) {
                // Reallocate hotkeys as keybindings may have changed
                generate_response_lines();
            } else if( action == "ANY_INPUT" ) {
                // Check real hotkeys
                const auto hotkey_it = std::find( response_hotkeys.begin(),
                                                  response_hotkeys.end(), evt );
                response_ind = std::distance( response_hotkeys.begin(), hotkey_it );
            } else if( action == "QUIT" ) {
                response_ind = get_best_quit_response();
            }
        } while( ( action != "ANY_INPUT" && action != "QUIT" ) || response_ind >= response_hotkeys.size() );
        okay = true;
        std::set<dialogue_consequence> consequences = responses[response_ind].get_consequences( *this );
        if( consequences.count( dialogue_consequence::hostile ) > 0 ) {
            okay = query_yn( _( "You may be attacked!  Proceed?" ) );
        } else if( consequences.count( dialogue_consequence::helpless ) > 0 ) {
            okay = query_yn( _( "You'll be helpless!  Proceed?" ) );
        }
    } while( !okay );

    d_win.add_history_separator();
    d_win.add_to_history( response_lines[response_ind].text, _( "You" ), c_light_blue );

    talk_response chosen = responses[response_ind];
    if( chosen.mission_selected != nullptr ) {
        actor( true )->select_mission( chosen.mission_selected );
    }

    // We can't set both skill and style or training will bug out
    // TODO: Allow setting both skill and style
    actor( true )->store_chosen_training( chosen.skill, chosen.style, chosen.dialogue_spell,
                                          chosen.proficiency );
    const bool success = chosen.trial.roll( *this );
    const auto &effects = success ? chosen.success : chosen.failure;
    return effects.apply( *this );
}

/**
 * Finds the best response to use when the player is trying to quit.
 *
 * Returns the index into the response list.
 */
int dialogue::get_best_quit_response() const
{
    if( responses.size() == 1 ) {
        // Only one response. Use it. Consequences will be prompted for by the caller.
        return 0;
    }

    // Find relevant responses
    for( size_t i = 0; i < responses.size(); ++i ) {
        const talk_response &response = responses[i];
        if( response.trial.calc_chance( *this ) < 100 ) {
            // Don't pick anything with a chance to fail.
            continue;
        }

        if( !response.success.effects.empty() ) {
            // Don't pick anything with side effects
            continue;
        }

        // Unfortunately, while we'd like to be able to go "back" from nested dialogue trees, the
        // topic stack doesn't always shrink. Returning to the previous topic is sometimes done
        // with TALK_NONE, or sometimes by referencing the topic id directly. No solution really
        // gives us something that feels right in all cases, so we only support completely leaving
        // the conversation via the quit key.

        if( response.success.next_topic.id == "TALK_DONE" ) {
            return i;
        }
    }

    return responses.size(); // Didn't find a good option
}

talk_trial::talk_trial( const JsonObject &jo )
{
    static const std::unordered_map<std::string, talk_trial_type> types_map = { {
#define WRAP(value) { #value, TALK_TRIAL_##value }
            WRAP( NONE ),
            WRAP( LIE ),
            WRAP( PERSUADE ),
            WRAP( INTIMIDATE ),
            WRAP( CONDITION )
#undef WRAP
        }
    };
    const auto iter = types_map.find( jo.get_string( "type", "NONE" ) );
    if( iter == types_map.end() ) {
        jo.throw_error( "invalid talk trial type", "type" );
    }
    type = iter->second;
    if( !( type == TALK_TRIAL_NONE || type == TALK_TRIAL_CONDITION ) ) {
        difficulty = jo.get_int( "difficulty" );
    }

    read_condition<dialogue>( jo, "condition", condition, false );

    if( jo.has_member( "mod" ) ) {
        for( JsonArray jmod : jo.get_array( "mod" ) ) {
            trial_mod this_modifier;
            this_modifier.first = jmod.next_string();
            this_modifier.second = jmod.next_int();
            modifiers.push_back( this_modifier );
        }
    }
}

static talk_topic load_inline_topic( const JsonObject &jo )
{
    const std::string id = jo.get_string( "id" );
    json_talk_topics[id].load( jo );
    return talk_topic( id );
}

talk_effect_fun_t::talk_effect_fun_t( const talkfunction_ptr &ptr )
{
    function = [ptr]( const dialogue & d ) {
        if( d.actor( true )->get_npc() ) {
            ptr( *d.actor( true )->get_npc() );
        }
    };
}

talk_effect_fun_t::talk_effect_fun_t( const std::function<void( npc &p )> &ptr )
{
    function = [ptr]( const dialogue & d ) {
        if( d.actor( true )->get_npc() ) {
            ptr( *d.actor( true )->get_npc() );
        }
    };
}

talk_effect_fun_t::talk_effect_fun_t( const std::function<void( const dialogue &d )> &fun )
{
    function = [fun]( const dialogue & d ) {
        fun( d );
    };
}

void talk_effect_fun_t::set_companion_mission( const std::string &role_id )
{
    function = [role_id]( const dialogue & d ) {
        d.actor( true )->set_companion_mission( role_id );
    };
}

void talk_effect_fun_t::set_add_effect( const JsonObject &jo, const std::string &member,
                                        bool is_npc )
{
    std::string new_effect = jo.get_string( member );
    bool permanent = false;
    bool force = false;
    duration_or_var dov_duration;
    int_or_var iov_intensity;
    if( jo.has_string( "duration" ) ) {
        const std::string dur_string = jo.get_string( "duration" );
        if( dur_string == "PERMANENT" ) {
            permanent = true;
            dov_duration = get_duration_or_var( jo, "", false, 0_turns );
        } else {
            dov_duration = get_duration_or_var( jo, "duration", false, 1000_turns );
        }
    } else {
        dov_duration = get_duration_or_var( jo, "duration", true );
    }
    iov_intensity = get_int_or_var( jo, "intensity", false, 0 );
    if( jo.has_bool( "force" ) ) {
        force = jo.get_bool( "force" );
    }
    std::string target = jo.get_string( "target_part", "bp_null" );
    function = [is_npc, new_effect, dov_duration, target, permanent, force,
            iov_intensity]( const dialogue & d ) {
        d.actor( is_npc )->add_effect( efftype_id( new_effect ), dov_duration.evaluate( d.actor( is_npc ) ),
                                       target, permanent, force,
                                       iov_intensity.evaluate( d.actor( is_npc ) ) );
    };
}

void talk_effect_fun_t::set_remove_effect( const JsonObject &jo, const std::string &member,
        bool is_npc )
{
    std::string old_effect = jo.get_string( member );
    function = [is_npc, old_effect]( const dialogue & d ) {
        d.actor( is_npc )->remove_effect( efftype_id( old_effect ) );
    };
}

void talk_effect_fun_t::set_add_trait( const JsonObject &jo, const std::string &member,
                                       bool is_npc )
{
    std::string new_trait = jo.get_string( member );
    function = [is_npc, new_trait]( const dialogue & d ) {
        d.actor( is_npc )->set_mutation( trait_id( new_trait ) );
    };
}

void talk_effect_fun_t::set_remove_trait( const JsonObject &jo, const std::string &member,
        bool is_npc )
{
    std::string old_trait = jo.get_string( member );
    function = [is_npc, old_trait]( const dialogue & d ) {
        d.actor( is_npc )->unset_mutation( trait_id( old_trait ) );
    };
}

void talk_effect_fun_t::set_add_bionic( const JsonObject &jo, const std::string &member,
                                        bool is_npc )
{
    std::string new_bionic = jo.get_string( member );
    function = [is_npc, new_bionic]( const dialogue & d ) {
        d.actor( is_npc )->add_bionic( bionic_id( new_bionic ) );
    };
}

void talk_effect_fun_t::set_lose_bionic( const JsonObject &jo, const std::string &member,
        bool is_npc )
{
    std::string old_bionic = jo.get_string( member );
    function = [is_npc, old_bionic]( const dialogue & d ) {
        d.actor( is_npc )->remove_bionic( bionic_id( old_bionic ) );
    };
}

void talk_effect_fun_t::set_add_var( const JsonObject &jo, const std::string &member,
                                     bool is_npc )
{
    const std::string var_name = get_talk_varname( jo, member );
    const bool time_check = jo.has_member( "time" ) && jo.get_bool( "time" );
    std::vector<std::string> possible_values = jo.get_string_array( "possible_values" );
    if( possible_values.empty() ) {
        const std::string value = time_check ? "" : jo.get_string( "value" );
        possible_values.push_back( value );
    }
    function = [is_npc, var_name, possible_values, time_check ]( const dialogue & d ) {
        talker *actor = d.actor( is_npc );
        if( time_check ) {
            actor->set_value( var_name, string_format( "%d", to_turn<int>( calendar::turn ) ) );
        } else {
            int index = rng( 0, possible_values.size() - 1 );
            actor->set_value( var_name, possible_values[index] );
        }
    };
}

void talk_effect_fun_t::set_remove_var( const JsonObject &jo, const std::string &member,
                                        bool is_npc )
{
    const std::string var_name = get_talk_varname( jo, member, false );
    function = [is_npc, var_name]( const dialogue & d ) {
        d.actor( is_npc )->remove_value( var_name );
    };
}

void talk_effect_fun_t::set_adjust_var( const JsonObject &jo, const std::string &member,
                                        bool is_npc )
{
    const std::string var_name = get_talk_varname( jo, member, false );
    int_or_var iov = get_int_or_var( jo, "adjustment" );
    function = [is_npc, var_name, iov]( const dialogue & d ) {
        int adjusted_value = iov.evaluate( d.actor( is_npc ) );

        const std::string &var = d.actor( is_npc )->get_value( var_name );
        if( !var.empty() ) {
            adjusted_value += std::stoi( var );
        }

        d.actor( is_npc )->set_value( var_name, std::to_string( adjusted_value ) );
    };
}

static void receive_item( const itype_id &item_name, int count, const std::string &container_name,
                          const dialogue &d )
{
    if( container_name.empty() ) {
        item new_item = item( item_name, calendar::turn );
        if( new_item.count_by_charges() ) {
            new_item.mod_charges( count - 1 );
            d.actor( false )->i_add( new_item );
        } else {
            for( int i_cnt = 0; i_cnt < count; i_cnt++ ) {
                if( !new_item.ammo_default().is_null() ) {
                    new_item.ammo_set( new_item.ammo_default() );
                }
                d.actor( false )->i_add( new_item );
            }
        }
        if( d.has_beta ) {
            if( count == 1 ) {
                //~ %1%s is the NPC name, %2$s is an item
                popup( _( "%1$s gives you a %2$s." ), d.actor( true )->disp_name(), new_item.tname() );
            } else {
                //~ %1%s is the NPC name, %2$d is a number of items, %3$s are items
                popup( _( "%1$s gives you %2$d %3$s." ), d.actor( true )->disp_name(), count,
                       new_item.tname() );
            }
        }
    } else {
        item container( container_name, calendar::turn );
        container.put_in( item( item_name, calendar::turn, count ),
                          item_pocket::pocket_type::CONTAINER );
        d.actor( false )->i_add( container );
        if( d.has_beta ) {
            //~ %1%s is the NPC name, %2$s is an item
            popup( _( "%1$s gives you a %2$s." ), d.actor( true )->disp_name(), container.tname() );
        }
    }
}

void talk_effect_fun_t::set_u_spawn_item( const itype_id &item_name, int count,
        const std::string &container_name )
{
    function = [item_name, count, container_name]( const dialogue & d ) {
        receive_item( item_name, count, container_name, d );
    };
    likely_rewards.emplace_back( count, item_name );
}

void talk_effect_fun_t::set_u_buy_item( const itype_id &item_name, int cost, int count,
                                        const std::string &container_name )
{
    function = [item_name, cost, count, container_name]( const dialogue & d ) {
        if( !d.actor( true )->buy_from( cost ) ) {
            popup( _( "You can't afford it!" ) );
            return;
        }
        receive_item( item_name, count, container_name, d );
    };
}

void talk_effect_fun_t::set_u_sell_item( const itype_id &item_name, int cost, int count )
{
    function = [item_name, cost, count]( const dialogue & d ) {
        if( item::count_by_charges( item_name ) && d.actor( false )->has_charges( item_name, count ) ) {
            for( const item &it : d.actor( false )->use_charges( item_name, count ) ) {
                d.actor( true )->i_add( it );
            }
        } else if( d.actor( false )->has_amount( item_name, count ) ) {
            for( const item &it : d.actor( false )->use_amount( item_name, count ) ) {
                d.actor( true )->i_add( it );
            }
        } else {
            //~ %1$s is a translated item name
            popup( _( "You don't have a %1$s!" ), item::nname( item_name ) );
            return;
        }
        if( count == 1 ) {
            //~ %1%s is the NPC name, %2$s is an item
            popup( _( "You give %1$s a %2$s." ), d.actor( true )->disp_name(), item::nname( item_name ) );
        } else {
            //~ %1%s is the NPC name, %2$d is a number of items, %3$s are items
            popup( _( "You give %1$s %2$d %3$s." ), d.actor( true )->disp_name(), count,
                   item::nname( item_name, count ) );
        }
        d.actor( true )->add_debt( cost );
    };
}

void talk_effect_fun_t::set_consume_item( const JsonObject &jo, const std::string &member,
        int count,
        bool is_npc )
{
    itype_id item_name;
    jo.read( member, item_name, true );
    function = [is_npc, item_name, count]( const dialogue & d ) {
        // this is stupid, but I couldn't get the assignment to work
        const auto consume_item = [&]( talker & p, const itype_id & item_name, int count ) {
            item old_item( item_name );
            if( p.has_charges( item_name, count ) ) {
                p.use_charges( item_name, count );
            } else if( p.has_amount( item_name, count ) ) {
                p.use_amount( item_name, count );
            } else {
                //~ %1%s is the "You" or the NPC name, %2$s are a translated item name
                popup( _( "%1$s doesn't have a %2$s!" ), p.disp_name(), old_item.tname() );
            }
        };
        if( is_npc ) {
            consume_item( *d.actor( true ), item_name, count );
        } else {
            consume_item( *d.actor( false ), item_name, count );
        }
    };
}

void talk_effect_fun_t::set_remove_item_with( const JsonObject &jo, const std::string &member,
        bool is_npc )
{
    const std::string &item_name = jo.get_string( member );
    function = [is_npc, item_name]( const dialogue & d ) {
        itype_id item_id = itype_id( item_name );
        d.actor( is_npc )->remove_items_with( [item_id]( const item & it ) {
            return it.typeId() == item_id;
        } );
    };
}

void talk_effect_fun_t::set_u_spend_cash( int amount )
{
    function = [amount]( const dialogue & d ) {
        d.actor( true )->buy_from( amount );
    };
}

void talk_effect_fun_t::set_npc_change_faction( const std::string &faction_name )
{
    function = [faction_name]( const dialogue & d ) {
        d.actor( true )->set_fac( faction_id( faction_name ) );
    };
}

void talk_effect_fun_t::set_npc_change_class( const std::string &class_name )
{
    function = [class_name]( const dialogue & d ) {
        d.actor( true )->set_class( npc_class_id( class_name ) );
    };
}

void talk_effect_fun_t::set_change_faction_rep( int rep_change )
{
    function = [rep_change]( const dialogue & d ) {
        d.actor( true )->add_faction_rep( rep_change );
    };
}

void talk_effect_fun_t::set_add_debt( const std::vector<trial_mod> &debt_modifiers )
{
    function = [debt_modifiers]( const dialogue & d ) {
        int debt = 0;
        for( const trial_mod &this_mod : debt_modifiers ) {
            if( this_mod.first == "TOTAL" ) {
                debt *= this_mod.second;
            } else {
                debt += parse_mod( d, this_mod.first, this_mod.second );
            }
        }
        d.actor( true )->add_debt( debt );
    };
}

void talk_effect_fun_t::set_toggle_npc_rule( const std::string &rule )
{
    function = [rule]( const dialogue & d ) {
        d.actor( true )->toggle_ai_rule( "ally_rule", rule );
    };
}

void talk_effect_fun_t::set_set_npc_rule( const std::string &rule )
{
    function = [rule]( const dialogue & d ) {
        d.actor( true )->set_ai_rule( "ally_rule", rule );
    };
}

void talk_effect_fun_t::set_clear_npc_rule( const std::string &rule )
{
    function = [rule]( const dialogue & d ) {
        d.actor( true )->clear_ai_rule( "ally_rule", rule );
    };
}

void talk_effect_fun_t::set_npc_engagement_rule( const std::string &setting )
{
    function = [setting]( const dialogue & d ) {
        d.actor( true )->set_ai_rule( "engagement_rule", setting );
    };
}

void talk_effect_fun_t::set_npc_aim_rule( const std::string &setting )
{
    function = [setting]( const dialogue & d ) {
        d.actor( true )->set_ai_rule( "aim_rule", setting );
    };
}

void talk_effect_fun_t::set_npc_cbm_reserve_rule( const std::string &setting )
{
    function = [setting]( const dialogue & d ) {
        d.actor( true )->set_ai_rule( "cbm_reserve_rule", setting );
    };
}

void talk_effect_fun_t::set_npc_cbm_recharge_rule( const std::string &setting )
{
    function = [setting]( const dialogue & d ) {
        d.actor( true )->set_ai_rule( "cbm_recharge_rule", setting );
    };
}

void talk_effect_fun_t::set_location_variable( const JsonObject &jo, const std::string &member,
        bool is_npc )
{
    JsonObject member_obj = jo.get_object( member );
    bool global = member_obj.get_bool( "global", false );
    std::string var_name = get_talk_varname( member_obj, "value" );
    int_or_var iov_min_radius = get_int_or_var( jo, "min_radius", false, 0 );
    int_or_var iov_max_radius = get_int_or_var( jo, "max_radius", false, 0 );
    const bool outdoor_only = jo.get_bool( "outdoor_only", false );
    function = [iov_min_radius, iov_max_radius, var_name, outdoor_only, global,
                    is_npc]( const dialogue & d ) {
        talker *target = d.actor( is_npc );
        tripoint talker_pos = get_map().getabs( target->pos() );
        tripoint target_pos = talker_pos;
        int max_radius = iov_max_radius.evaluate( target );
        if( max_radius > 0 ) {
            bool found = false;
            int min_radius = iov_min_radius.evaluate( target );
            for( int attempts = 0; attempts < 25; attempts++ ) {
                target_pos = talker_pos + tripoint( rng( -max_radius, max_radius ), rng( -max_radius, max_radius ),
                                                    0 );
                if( ( !outdoor_only || get_map().is_outside( target_pos ) ) &&
                    rl_dist( target_pos, talker_pos ) >= min_radius ) {
                    found = true;
                    break;
                }
            }
            if( !found ) {
                return;
            }
        }
        if( global ) {
            global_variables &globvars = get_globals();
            globvars.set_global_value( var_name, target_pos.to_string() );
        } else {
            target->set_value( var_name, target_pos.to_string() );
        }
    };
}

void talk_effect_fun_t::set_transform_radius( const JsonObject &jo, const std::string &member,
        bool is_npc )
{
    ter_furn_transform_id transform = ter_furn_transform_id( jo.get_string( "ter_furn_transform" ) );
    int_or_var iov = get_int_or_var( jo, member );
    cata::optional<std::string> target_var;
    bool global = false;
    if( jo.has_member( "target_var" ) ) {
        JsonObject target_obj = jo.get_object( "target_var" );
        global = target_obj.get_bool( "global", false );
        target_var = get_talk_varname( target_obj, "value" );
    }
    function = [iov, transform, target_var, global, is_npc]( const dialogue & d ) {
        talker *target = d.actor( is_npc );
        tripoint target_pos = get_tripoint_from_var( target, target_var, global );
        get_map().transform_radius( transform, iov.evaluate( target ), target_pos );
    };
}

void talk_effect_fun_t::set_mapgen_update( const JsonObject &jo, const std::string &member )
{
    mission_target_params target_params = mission_util::parse_mission_om_target( jo );
    std::vector<update_mapgen_id> update_ids;

    if( jo.has_string( member ) ) {
        update_ids.emplace_back( update_mapgen_id( jo.get_string( member ) ) );
    } else if( jo.has_array( member ) ) {
        for( const std::string line : jo.get_array( member ) ) {
            update_ids.emplace_back( line );
        }
    }

    function = [target_params, update_ids]( const dialogue & d ) {
        mission_target_params update_params = target_params;
        if( d.has_beta ) {
            update_params.guy = d.actor( true )->get_npc();
        }
        const tripoint_abs_omt omt_pos = mission_util::get_om_terrain_pos( update_params );
        for( const update_mapgen_id &mapgen_update_id : update_ids ) {
            run_mapgen_update_func( mapgen_update_id, omt_pos, d.actor( d.has_beta )->selected_mission() );
        }
    };
}

void talk_effect_fun_t::set_npc_goal( const JsonObject &jo, const std::string &member )
{
    mission_target_params dest_params = mission_util::parse_mission_om_target( jo.get_object(
                                            member ) );
    function = [dest_params]( const dialogue & d ) {
        npc *guy = d.actor( true )->get_npc();
        if( guy ) {
            tripoint_abs_omt destination = mission_util::get_om_terrain_pos( dest_params );
            guy->goal = destination;
            guy->omt_path = overmap_buffer.get_travel_path( guy->global_omt_location(), guy->goal,
                            overmap_path_params::for_npc() );
            if( destination == tripoint_abs_omt() || destination == overmap::invalid_tripoint ||
                guy->omt_path.empty() ) {
                guy->goal = npc::no_goal_point;
                guy->omt_path.clear();
                return;
            }
            guy->set_mission( NPC_MISSION_TRAVELLING );
            guy->guard_pos = cata::nullopt;
            guy->set_attitude( NPCATT_NULL );
        }
    };
}

void talk_effect_fun_t::set_bulk_trade_accept( bool is_trade, int quantity, bool is_npc )
{
    function = [is_trade, is_npc, quantity]( const dialogue & d ) {
        talker *seller = d.actor( is_npc );
        talker *buyer = d.actor( !is_npc );
        item tmp( d.cur_item );
        int seller_has = 0;
        if( tmp.count_by_charges() ) {
            seller_has = seller->charges_of( d.cur_item );
        } else {
            seller_has = seller->items_with( [&tmp]( const item & e ) {
                return tmp.type == e.type;
            } ).size();
        }
        seller_has = ( quantity == -1 ) ? seller_has : std::min( seller_has, quantity );
        tmp.charges = seller_has;
        if( is_trade ) {
            const int npc_debt = d.actor( true )->debt();
            int price = tmp.price( true ) * ( is_npc ? -1 : 1 ) + npc_debt;
            if( d.actor( true )->get_faction() && !d.actor( true )->get_faction()->currency.is_empty() ) {
                const itype_id &pay_in = d.actor( true )->get_faction()->currency;
                item pay( pay_in );
                const int value = d.actor( true )->value( pay );
                if( value > 0 ) {
                    int required = price / value;
                    int buyer_has = required;
                    if( is_npc ) {
                        buyer_has = std::min( buyer_has, buyer->charges_of( pay_in ) );
                        buyer->use_charges( pay_in, buyer_has );
                    } else {
                        if( buyer_has == 1 ) {
                            //~ %1%s is the NPC name, %2$s is an item
                            popup( _( "%1$s gives you a %2$s." ), seller->disp_name(),
                                   pay.tname() );
                        } else if( buyer_has > 1 ) {
                            //~ %1%s is the NPC name, %2$d is a number of items, %3$s are items
                            popup( _( "%1$s gives you %2$d %3$s." ), seller->disp_name(), buyer_has,
                                   pay.tname() );
                        }
                    }
                    for( int i = 0; i < buyer_has; i++ ) {
                        seller->i_add( pay );
                        price -= value;
                    }
                } else {
                    debugmsg( "%s pays in bulk_trade_accept with faction currency worth 0!",
                              d.actor( true )->disp_name() );
                }
            } else {
                debugmsg( "%s has no faction currency to pay with in bulk_trade_accept!",
                          d.actor( true )->disp_name() );
            }
            d.actor( true )->add_debt( -npc_debt );
            d.actor( true )->add_debt( price );
        }
        if( tmp.count_by_charges() ) {
            seller->use_charges( d.cur_item, seller_has );
        } else {
            seller->use_amount( d.cur_item, seller_has );
        }
        buyer->i_add( tmp );
    };
}

void talk_effect_fun_t::set_npc_gets_item( bool to_use )
{
    function = [to_use]( const dialogue & d ) {
        d.reason = d.actor( true )->give_item_to( to_use );
    };
}

void talk_effect_fun_t::set_add_mission( const std::string &mission_id )
{
    function = [mission_id]( const dialogue & d ) {
        d.actor( true )->add_mission( mission_type_id( mission_id ) );
    };
}

const std::vector<std::pair<int, itype_id>> &talk_effect_fun_t::get_likely_rewards() const
{
    return likely_rewards;
}

void talk_effect_fun_t::set_u_buy_monster( const std::string &monster_type_id, int cost, int count,
        bool pacified, const translation &name )
{

    function = [monster_type_id, cost, count, pacified, name]( const dialogue & d ) {
        const mtype_id mtype( monster_type_id );
        d.actor( false )->buy_monster( *d.actor( true ), mtype, cost, count, pacified, name );
    };
}

void talk_effect_fun_t::set_u_learn_recipe( const std::string &learned_recipe_id )
{
    function = [learned_recipe_id]( const dialogue & ) {
        const recipe &r = recipe_id( learned_recipe_id ).obj();
        get_player_character().learn_recipe( &r );
        popup( _( "You learn how to craft %s." ), r.result_name() );
    };
}

void talk_effect_fun_t::set_npc_first_topic( const std::string &chat_topic )
{
    function = [chat_topic]( const dialogue & d ) {
        d.actor( true )->set_first_topic( chat_topic );
    };
}

void talk_effect_fun_t::set_message( const JsonObject &jo, const std::string &member, bool is_npc )
{
    std::string message = jo.get_string( member );
    const bool snippet = jo.get_bool( "snippet", false );
    const bool same_snippet = jo.get_bool( "same_snippet", false );
    const bool outdoor_only = jo.get_bool( "outdoor_only", false );
    const bool sound = jo.get_bool( "sound", false );
    const bool popup_msg = jo.get_bool( "popup", false );
    game_message_type type = m_neutral;
    std::string type_string = jo.get_string( "type", "neutral" );
    if( type_string == "good" ) {
        type = m_good;
    } else if( type_string == "neutral" ) {
        type = m_neutral;
    } else if( type_string == "bad" ) {
        type = m_bad;
    } else if( type_string == "mixed" ) {
        type = m_mixed;
    } else if( type_string == "warning" ) {
        type = m_warning;
    } else if( type_string == "info" ) {
        type = m_info;
    } else if( type_string == "debug" ) {
        type = m_debug;
    } else if( type_string == "headshot" ) {
        type = m_headshot;
    } else if( type_string == "critical" ) {
        type = m_critical;
    } else if( type_string == "grazing" ) {
        type = m_grazing;
    } else {
        jo.throw_error( "Invalid message type." );
    }

    function = [message, outdoor_only, sound, snippet, same_snippet, type, popup_msg,
             is_npc]( const dialogue & d ) {
        Character *target = d.actor( is_npc )->get_character();
        if( !target || target->is_npc() ) {
            return;
        }
        std::string translated_message;
        if( snippet ) {
            if( same_snippet ) {
                talker *target = d.actor( !is_npc );
                std::string sid = target->get_value( message + "_snippet_id" );
                if( sid.empty() ) {
                    sid = SNIPPET.random_id_from_category( message ).c_str();
                    target->set_value( message + "_snippet_id", sid );
                }
                translated_message = SNIPPET.expand( SNIPPET.get_snippet_by_id( snippet_id( sid ) ).value_or(
                        translation() ).translated() );
            } else {
                translated_message = SNIPPET.expand( SNIPPET.random_from_category( message ).value_or(
                        translation() ).translated() );
            }
        } else {
            translated_message = _( message );
        }
        if( sound ) {
            bool display = false;
            map &here = get_map();
            if( !target->has_effect( effect_sleep ) && !target->is_deaf() ) {
                if( !outdoor_only || here.get_abs_sub().z >= 0 ||
                    one_in( std::max( roll_remainder( 2.0f * here.get_abs_sub().z /
                                                      target->mutation_value( "hearing_modifier" ) ), 1 ) ) ) {
                    display = true;
                }
            }
            if( !display ) {
                return;
            }
        }
        if( popup_msg ) {
            const auto new_win = [translated_message]() {
                query_popup pop;
                pop.message( "%s", translated_message );
                return pop.get_window();
            };
            scrollable_text( new_win, "", replace_colors( translated_message ) );
        } else {
            target->add_msg_if_player( type, translated_message );
        }

    };
}

void talk_effect_fun_t::set_assign_activity( const JsonObject &jo, const std::string &member,
        bool is_npc )
{
    duration_or_var dov = get_duration_or_var( jo, "duration", true );
    activity_id act = activity_id( jo.get_string( member ) );
    function = [is_npc, dov, act]( const dialogue & d ) {
        Character *target = d.actor( is_npc )->get_character();
        if( target ) {
            target->assign_activity( act, to_moves<int>( dov.evaluate( d.actor( is_npc ) ) ) );
        }
    };
}

void talk_effect_fun_t::set_add_wet( const JsonObject &jo, const std::string &member,
                                     bool is_npc )
{
    int_or_var iov = get_int_or_var( jo, member );
    function = [is_npc, iov]( const dialogue & d ) {
        Character *target = d.actor( is_npc )->get_character();
        if( target ) {
            wet_character( *target, iov.evaluate( d.actor( is_npc ) ) );
        }
    };
}

void talk_effect_fun_t::set_open_dialogue()
{
    function = []( const dialogue & d ) {
        if( !d.actor( false )->get_character()->is_avatar() ) { //only open a dialog if the avatar is alpha
            return;
        } else if( d.actor( true )->get_character() != nullptr ) {
            get_avatar().talk_to( get_talker_for( d.actor( true )->get_character() ) );
        } else if( d.actor( true )->get_creature() != nullptr ) {
            get_avatar().talk_to( get_talker_for( d.actor( true )->get_creature() ) );
        } else if( d.actor( true )->get_monster() != nullptr ) {
            get_avatar().talk_to( get_talker_for( d.actor( true )->get_monster() ) );
        } else if( d.actor( true )->get_item() != nullptr ) {
            get_avatar().talk_to( get_talker_for( d.actor( true )->get_item() ) );
        } else if( d.actor( true )->get_computer() != nullptr ) {
            get_avatar().talk_to( get_talker_for( d.actor( true )->get_computer() ), false, true );
        }
    };
}

void talk_effect_fun_t::set_take_control()
{
    function = []( const dialogue & d ) {
        if( !d.actor( false )->get_character()->is_avatar() ) { //only take control if the avatar is alpha
            return;
        } else if( d.actor( true )->get_npc() != nullptr ) {
            get_avatar().control_npc( *d.actor( true )->get_npc() );
        }
    };
}

void talk_effect_fun_t::set_take_control_menu()
{
    function = []( const dialogue & ) {
        get_avatar().control_npc_menu();
    };
}

void talk_effect_fun_t::set_sound_effect( const JsonObject &jo, const std::string &member )
{
    std::string variant = jo.get_string( member );
    std::string id = jo.get_string( "id" );
    const bool outdoor_event = jo.get_bool( "outdoor_event", false );
    const int volume = jo.get_int( "volume", -1 );
    function = [variant, id, outdoor_event, volume]( const dialogue & ) {
        map &here = get_map();
        int local_volume = volume;
        Character *target = &get_player_character(); //Only the player can hear sound effects.
        if( target && !target->has_effect( effect_sleep ) && !target->is_deaf() ) {
            if( !outdoor_event || here.get_abs_sub().z >= 0 ) {
                if( local_volume == -1 ) {
                    local_volume = 80;
                }
                sfx::play_variant_sound( id, variant, local_volume, random_direction() );
            } else if( one_in( std::max( roll_remainder( 2.0f * here.get_abs_sub().z /
                                         target->mutation_value( "hearing_modifier" ) ), 1 ) ) ) {
                if( local_volume == -1 ) {
                    local_volume = 80 * target->mutation_value( "hearing_modifier" );
                }
                sfx::play_variant_sound( id, variant, local_volume, random_direction() );
            }
        }
    };
}

void talk_effect_fun_t::set_mod_healthy( const JsonObject &jo, const std::string &member,
        bool is_npc )
{
    int_or_var iov_amount = get_int_or_var( jo, member );
    int_or_var iov_cap = get_int_or_var( jo, "cap" );

    function = [is_npc, iov_amount, iov_cap]( const dialogue & d ) {
        d.actor( is_npc )->mod_healthy_mod( iov_amount.evaluate( d.actor( is_npc ) ),
                                            iov_cap.evaluate( d.actor( is_npc ) ) );
    };
}

void talk_effect_fun_t::set_cast_spell( const JsonObject &jo, const std::string &member,
                                        bool is_npc )
{
    fake_spell fake;
    mandatory( jo, false, member, fake );
    function = [is_npc, fake]( const dialogue & d ) {
        Creature *caster = d.actor( is_npc )->get_creature();
        if( !caster ) {
            debugmsg( "No valid caster for spell." );
        } else {
            fake.get_spell( 0 ).cast_all_effects( *caster, caster->pos() );
        }
    };
}

void talk_effect_fun_t::set_lightning()
{
    function = []( const dialogue & ) {
        if( get_player_character().posz() >= 0 ) {
            get_weather().lightning_active = true;
        }
    };
}

void talk_effect_fun_t::set_next_weather()
{
    function = []( const dialogue & ) {
        get_weather().set_nextweather( calendar::turn );
    };
}

void talk_effect_fun_t::set_arithmetic( const JsonObject &jo, const std::string &member )
{
    JsonArray objects = jo.get_array( member );
    std::string op = "none";
    std::string result = "none";
    std::function<void( const dialogue &, int )> set_int = get_set_int( objects.get_object( 0 ) );
    // Normal full version
    if( objects.size() == 5 ) {
        op = objects.get_string( 3 );
        result = objects.get_string( 1 );
        if( result != "=" ) {
            jo.throw_error( "invalid result " + op + " in " + jo.str() );
            function = []( const dialogue & ) {
                return false;
            };
        }
        std::function<int( const dialogue & )> get_first_int = conditional_t< dialogue >::get_get_int(
                    objects.get_object( 2 ) );
        std::function<int( const dialogue & )> get_second_int = conditional_t< dialogue >::get_get_int(
                    objects.get_object( 4 ) );
        if( op == "*" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) * get_second_int( d ) );
            };
        } else if( op == "/" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) / get_second_int( d ) );
            };
        } else if( op == "+" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) + get_second_int( d ) );
            };
        } else if( op == "-" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) - get_second_int( d ) );
            };
        } else if( op == "%" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) % get_second_int( d ) );
            };
        } else if( op == "&" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) & get_second_int( d ) );
            };
        } else if( op == "|" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) | get_second_int( d ) );
            };
        } else if( op == "<<" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) << get_second_int( d ) );
            };
        } else if( op == ">>" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) >> get_second_int( d ) );
            };
        } else if( op == "^" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) ^ get_second_int( d ) );
            };
        } else {
            jo.throw_error( "unexpected operator " + op + " in " + jo.str() );
            function = []( const dialogue & ) {
                return false;
            };
        }
        // ~
    } else if( objects.size() == 4 ) {
        op = objects.get_string( 3 );
        result = objects.get_string( 1 );
        if( result != "=" ) {
            jo.throw_error( "invalid result " + op + " in " + jo.str() );
            function = []( const dialogue & ) {
                return false;
            };
        }
        std::function<int( const dialogue & )> get_first_int = conditional_t< dialogue >::get_get_int(
                    objects.get_object( 2 ) );
        if( op == "~" ) {
            function = [get_first_int, set_int]( const dialogue & d ) {
                set_int( d, ~get_first_int( d ) );
            };
        } else {
            jo.throw_error( "unexpected operator " + op + " in " + jo.str() );
            function = []( const dialogue & ) {
                return false;
            };
        }

        // =, -=, +=, *=, and /=
    } else if( objects.size() == 3 ) {
        result = objects.get_string( 1 );
        std::function<int( const dialogue & )> get_first_int = conditional_t< dialogue >::get_get_int(
                    objects.get_object( 0 ) );
        std::function<int( const dialogue & )> get_second_int = conditional_t< dialogue >::get_get_int(
                    objects.get_object( 2 ) );
        if( result == "+=" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) + get_second_int( d ) );
            };
        } else if( result == "-=" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) - get_second_int( d ) );
            };
        } else if( result == "*=" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) * get_second_int( d ) );
            };
        } else if( result == "/=" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) / get_second_int( d ) );
            };
        } else if( result == "%=" ) {
            function = [get_first_int, get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) % get_second_int( d ) );
            };
        } else if( result == "=" ) {
            function = [get_second_int, set_int]( const dialogue & d ) {
                set_int( d, get_second_int( d ) );
            };
        } else {
            jo.throw_error( "unexpected result " + result + " in " + jo.str() );
            function = []( const dialogue & ) {
                return false;
            };
        }
        // ++ and --
    } else if( objects.size() == 2 ) {
        op = objects.get_string( 1 );
        std::function<int( const dialogue & )> get_first_int = conditional_t< dialogue >::get_get_int(
                    objects.get_object( 0 ) );
        if( op == "++" ) {
            function = [get_first_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) + 1 );
            };
        } else if( op == "--" ) {
            function = [get_first_int, set_int]( const dialogue & d ) {
                set_int( d, get_first_int( d ) - 1 );
            };
        } else {
            jo.throw_error( "unexpected operator " + op + " in " + jo.str() );
            function = []( const dialogue & ) {
                return false;
            };
        }
    } else {
        jo.throw_error( "Invalid number of args in " + jo.str() );
        function = []( const dialogue & ) {
            return false;
        };
        return;
    }
}

std::function<void( const dialogue &, int )> talk_effect_fun_t::get_set_int( const JsonObject &jo )
{
    if( jo.has_member( "const" ) ) {
        jo.throw_error( "attempted to alter a constant value in " + jo.str() );
    } else if( jo.has_member( "time" ) ) {
        jo.throw_error( "can not alter a time constant.  Did you mean time_since_cataclysm or time_since_var?  In "
                        + jo.str() );
    } else if( jo.has_member( "time_since_cataclysm" ) ) {
        time_duration given_unit = 1_turns;
        if( jo.has_string( "time_since_cataclysm" ) ) {
            std::string given_unit_str = jo.get_string( "time_since_cataclysm" );
            bool found = false;
            for( const auto &pair : time_duration::units ) {
                const std::string &unit = pair.first;
                if( unit == given_unit_str ) {
                    given_unit = pair.second;
                    found = true;
                    break;
                }
            }
            if( !found ) {
                jo.throw_error( "unrecognized time unit in " + jo.str() );
            }
        }
        return [given_unit]( const dialogue &, int input ) {
            calendar::turn = time_point( input * to_turns<int>( given_unit ) );
        };
    } else if( jo.has_member( "rand" ) ) {
        jo.throw_error( "can not alter the random number generator, silly!  In " + jo.str() );
    } else if( jo.has_member( "weather" ) ) {
        std::string weather_aspect = jo.get_string( "weather" );
        if( weather_aspect == "temperature" ) {
            return []( const dialogue &, int input ) {
                get_weather().weather_precise->temperature = input;
                get_weather().clear_temp_cache();
            };
        } else if( weather_aspect == "windpower" ) {
            return []( const dialogue &, int input ) {
                get_weather().weather_precise->windpower = input;
                get_weather().clear_temp_cache();
            };
        } else if( weather_aspect == "humidity" ) {
            return []( const dialogue &, int input ) {
                get_weather().weather_precise->humidity = input;
                get_weather().clear_temp_cache();
            };
        } else if( weather_aspect == "pressure" ) {
            return []( const dialogue &, int input ) {
                get_weather().weather_precise->pressure = input;
                get_weather().clear_temp_cache();
            };
        }
    } else if( jo.has_member( "u_val" ) || jo.has_member( "npc_val" ) ||
               jo.has_member( "global_val" ) ) {
        const bool is_npc = jo.has_member( "npc_val" );
        const bool is_global = jo.has_member( "global_val" );
        const std::string checked_value = is_npc ? jo.get_string( "npc_val" ) : is_global ?
                                          jo.get_string( "global_val" ) : jo.get_string( "u_val" );
        if( checked_value == "strength_base" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_str_max( input );
            };
        } else if( checked_value == "dexterity_base" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_dex_max( input );
            };
        } else if( checked_value == "intelligence_base" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_int_max( input );
            };
        } else if( checked_value == "perception_base" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_per_max( input );
            };
        } else if( checked_value == "var" ) {
            const std::string var_name = get_talk_varname( jo, "var_name", false );
            return [is_npc, var_name, is_global]( const dialogue & d, int input ) {
                if( is_global ) {
                    global_variables &globvars = get_globals();
                    globvars.set_global_value( var_name, std::to_string( input ) );
                } else {
                    d.actor( is_npc )->set_value( var_name, std::to_string( input ) );
                }
            };
        } else if( checked_value == "time_since_var" ) {
            // This is a strange thing to want to adjust. But we allow it nevertheless.
            const std::string var_name = get_talk_varname( jo, "var_name", false );
            return [is_npc, var_name]( const dialogue & d, int input ) {
                int storing_value = to_turn<int>( calendar::turn ) - input;
                d.actor( is_npc )->set_value( var_name, std::to_string( storing_value ) );
            };
        } else if( checked_value == "allies" ) {
            // It would be possible to make this work by removing allies and spawning new ones as needed.
            // But why would you ever want to do it this way?
            jo.throw_error( "altering allies this way is currently not supported.  In " + jo.str() );
        } else if( checked_value == "cash" ) {
            // TODO: See if this can be handeled in a clever way.
            jo.throw_error( "altering cash this way is currently not supported.  In " + jo.str() );
        } else if( checked_value == "owed" ) {
            if( is_npc ) {
                jo.throw_error( "owed amount not supported for NPCs.  In " + jo.str() );
            } else {
                return []( const dialogue & d, int input ) {
                    d.actor( true )->add_debt( input - d.actor( true )->debt() );
                };
            }
        } else if( checked_value == "sold" ) {
            if( is_npc ) {
                jo.throw_error( "sold amount not supported for NPCs.  In " + jo.str() );
            } else {
                return []( const dialogue & d, int input ) {
                    d.actor( true )->add_sold( input - d.actor( true )->sold() );
                };
            }
        } else if( checked_value == "skill_level" ) {
            const skill_id skill( jo.get_string( "skill" ) );
            return [is_npc, skill]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_skill_level( skill, input );
            };
        } else if( checked_value == "pos_x" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_pos( tripoint( input, d.actor( is_npc )->posy(),
                                                      d.actor( is_npc )->posz() ) );
            };
        } else if( checked_value == "pos_y" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_pos( tripoint( d.actor( is_npc )->posx(), input,
                                                      d.actor( is_npc )->posz() ) );
            };
        } else if( checked_value == "pos_z" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_pos( tripoint( d.actor( is_npc )->posx(), d.actor( is_npc )->posy(),
                                                      input ) );
            };
        } else if( checked_value == "pain" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->mod_pain( input - d.actor( is_npc )->pain_cur() );
            };
        } else if( checked_value == "power" ) {
            return [is_npc]( const dialogue & d, int input ) {
                // Energy in milijoule
                d.actor( is_npc )->set_power_cur( 1_mJ * input );
            };
        } else if( checked_value == "power_max" ) {
            jo.throw_error( "altering max power this way is currently not supported.  In " + jo.str() );
        } else if( checked_value == "power_percentage" ) {
            return [is_npc]( const dialogue & d, int input ) {
                // Energy in milijoule
                d.actor( is_npc )->set_power_cur( ( d.actor( is_npc )->power_max() * input ) / 100 );
            };
        } else if( checked_value == "focus" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->mod_focus( input - d.actor( is_npc )->focus_cur() );
            };
        } else if( checked_value == "mana" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_mana_cur( input );
            };
        } else if( checked_value == "mana_max" ) {
            jo.throw_error( "altering max mana this way is currently not supported.  In " + jo.str() );
        } else if( checked_value == "mana_percentage" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_mana_cur( ( d.actor( is_npc )->mana_max() * input ) / 100 );
            };
        } else if( checked_value == "hunger" ) {
            jo.throw_error( "altering hunger this way is currently not supported.  In " + jo.str() );
        } else if( checked_value == "thirst" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_thirst( input );
            };
        } else if( checked_value == "stored_kcal" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_stored_kcal( input );
            };
        } else if( checked_value == "stored_kcal_percentage" ) {
            // 100% is 55'000 kcal, which is considered healthy.
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_stored_kcal( input * 5500 );
            };
        } else if( checked_value == "item_count" ) {
            jo.throw_error( "altering items this way is currently not supported.  In " + jo.str() );
        } else if( checked_value == "exp" ) {
            jo.throw_error( "altering max exp this way is currently not supported.  In " + jo.str() );
        } else if( checked_value == "stim" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_stim( input );
            };
        } else if( checked_value == "pkill" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_pkill( input );
            };
        } else if( checked_value == "rad" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_rad( input );
            };
        } else if( checked_value == "fatigue" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_fatigue( input );
            };
        } else if( checked_value == "stamina" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_stamina( input );
            };
        } else if( checked_value == "sleep_deprivation" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_sleep_deprivation( input );
            };
        } else if( checked_value == "anger" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_anger( input );
            };
        } else if( checked_value == "morale" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_morale( input );
            };
        } else if( checked_value == "friendly" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_friendly( input );
            };
        } else if( checked_value == "exp" ) {
            return [is_npc]( const dialogue & d, int input ) {
                d.actor( is_npc )->set_kill_xp( input );
            };
        }
    }
    jo.throw_error( "error setting integer destination in " + jo.str() );
    return []( const dialogue &, int ) {};
}

void talk_effect_fun_t::set_assign_mission( const JsonObject &jo, const std::string &member )
{
    std::string mission_name = jo.get_string( member );
    function = [mission_name]( const dialogue & ) {
        avatar &player_character = get_avatar();

        const mission_type_id &mission_type = mission_type_id( mission_name );
        mission *new_mission = mission::reserve_new( mission_type, character_id() );
        new_mission->assign( player_character );
    };
}

void talk_effect_fun_t::set_finish_mission( const JsonObject &jo, const std::string &member )
{
    std::string mission_name = jo.get_string( member );
    bool success = false;
    cata::optional<int> step;
    if( jo.has_int( "step" ) ) {
        step = jo.get_int( "step" );
    } else {
        success = jo.get_bool( "success" );
    }
    function = [mission_name, success, step]( const dialogue & ) {
        avatar &player_character = get_avatar();

        const mission_type_id &mission_type = mission_type_id( mission_name );
        std::vector<mission *> missions = player_character.get_active_missions();
        for( mission *mission : missions ) {
            if( mission->mission_id() == mission_type ) {
                if( step.has_value() ) {
                    mission->step_complete( step.value() );
                } else if( success ) {
                    mission->wrap_up();
                } else {
                    mission->fail();
                }
                break;
            }
        }
    };
}

void talk_effect_fun_t::set_make_sound( const JsonObject &jo, const std::string &member,
                                        bool is_npc )
{
    std::string message = jo.get_string( member );

    int volume;
    mandatory( jo, false, "volume", volume );
    bool snippet = jo.get_bool( "snippet", false );
    bool same_snippet = jo.get_bool( "same_snippet", false );
    sounds::sound_t type = sounds::sound_t::background;
    std::string type_string = jo.get_string( "type", "background" );
    if( type_string == "background" ) {
        type = sounds::sound_t::background;
    } else if( type_string == "weather" ) {
        type = sounds::sound_t::weather;
    } else if( type_string == "music" ) {
        type = sounds::sound_t::music;
    } else if( type_string == "movement" ) {
        type = sounds::sound_t::movement;
    } else if( type_string == "speech" ) {
        type = sounds::sound_t::speech;
    } else if( type_string == "electronic_speech" ) {
        type = sounds::sound_t::electronic_speech;
    } else if( type_string == "activity" ) {
        type = sounds::sound_t::activity;
    } else if( type_string == "destructive_activity" ) {
        type = sounds::sound_t::destructive_activity;
    } else if( type_string == "alarm" ) {
        type = sounds::sound_t::alarm;
    } else if( type_string == "combat" ) {
        type = sounds::sound_t::combat;
    } else if( type_string == "alert" ) {
        type = sounds::sound_t::alert;
    } else if( type_string == "order" ) {
        type = sounds::sound_t::order;
    } else {
        jo.throw_error( "Invalid message type." );
    }
    cata::optional<std::string> target_var;
    bool global = false;
    if( jo.has_member( "target_var" ) ) {
        JsonObject target_obj = jo.get_object( "target_var" );
        global = target_obj.get_bool( "global", false );
        target_var = get_talk_varname( target_obj, "value" );
    }
    function = [is_npc, message, volume, type, target_var, global, snippet,
            same_snippet]( const dialogue & d ) {
        talker *target = d.actor( is_npc );
        tripoint target_pos = get_tripoint_from_var( target, target_var, global );
        std::string translated_message;
        if( snippet ) {
            if( same_snippet ) {
                talker *target = d.actor( !is_npc );
                std::string sid = target->get_value( message + "_snippet_id" );
                if( sid.empty() ) {
                    sid = SNIPPET.random_id_from_category( message ).c_str();
                    target->set_value( message + "_snippet_id", sid );
                }
                translated_message = SNIPPET.expand( SNIPPET.get_snippet_by_id( snippet_id( sid ) ).value_or(
                        translation() ).translated() );
            } else {
                translated_message = SNIPPET.expand( SNIPPET.random_from_category( message ).value_or(
                        translation() ).translated() );
            }
        } else {
            translated_message = _( message );
        }
        sounds::sound( get_map().getlocal( target_pos ), volume, type, translated_message );
    };
}

void talk_effect_fun_t::set_queue_effect_on_condition( const JsonObject &jo,
        const std::string &member, bool is_npc )
{
    std::vector<effect_on_condition_id> eocs;
    for( JsonValue jv : jo.get_array( member ) ) {
        eocs.push_back( effect_on_conditions::load_inline_eoc( jv, "" ) );
    }
    duration_or_var dov_time_in_future_min = get_duration_or_var( jo, "time_in_future_min", false,
            0_seconds );
    duration_or_var dov_time_in_future_max = get_duration_or_var( jo, "time_in_future_max", false,
            0_seconds );
    bool affect_nearby_npcs = jo.get_bool( "affect_nearby_npcs", false );
    std::vector<std::string> names = jo.get_string_array( "npcs_to_affect" );
    cata::optional<int> npc_range;
    if( jo.has_int( "npc_range" ) ) {
        npc_range = jo.get_int( "npc_range" );
    }

    bool npc_must_see = jo.get_bool( "npc_must_see", false );
    function = [dov_time_in_future_min, dov_time_in_future_max, eocs, names, npc_must_see, npc_range,
                            affect_nearby_npcs, is_npc]( const dialogue & d ) {
        time_duration max = dov_time_in_future_max.evaluate( d.actor( false ) );
        if( max > 0_seconds ) {
            time_duration time_in_future = rng( dov_time_in_future_min.evaluate( d.actor( false ) ), max );
            for( const effect_on_condition_id &eoc : eocs ) {
                if( eoc->type == eoc_type::ACTIVATION ) {
                    effect_on_conditions::queue_effect_on_condition( time_in_future, eoc, get_player_character() );
                } else {
                    debugmsg( "Cannot queue a non activation effect_on_condition." );
                }
            }
        } else {
            if( affect_nearby_npcs ) {
                tripoint actor_pos = d.actor( is_npc )->pos();
                const std::vector<npc *> available = g->get_npcs_if( [npc_must_see, npc_range, actor_pos,
                              names]( const npc & guy ) {
                    bool name_valid = names.empty();
                    for( const std::string &name : names ) {
                        if( name == guy.name ) {
                            name_valid = true;
                            break;
                        }
                    }
                    return name_valid && ( !npc_range.has_value() || actor_pos.z == guy.posz() ) && ( !npc_must_see ||
                            guy.sees( actor_pos ) ) &&
                           ( !npc_range.has_value() || rl_dist( actor_pos, guy.pos() ) <= npc_range.value() );
                } );
                for( npc *target : available ) {
                    for( const effect_on_condition_id &eoc : eocs ) {
                        dialogue newDialog( get_talker_for( target ), nullptr );
                        eoc->activate( newDialog );
                    }
                }
            } else {
                Creature *creature_alpha = d.has_alpha ? d.actor( false )->get_creature() : nullptr;
                item_location *item_alpha = d.has_alpha ? d.actor( false )->get_item() : nullptr;
                Creature *creature_beta = d.has_beta ? d.actor( true )->get_creature() : nullptr;
                item_location *item_beta = d.has_beta ? d.actor( true )->get_item() : nullptr;
                dialogue newDialog(
                    creature_alpha ? get_talker_for( creature_alpha ) : item_alpha ? get_talker_for(
                        item_alpha ) : nullptr,
                    creature_beta ? get_talker_for( creature_beta ) : item_beta ? get_talker_for(
                        item_beta ) : nullptr
                );
                for( const effect_on_condition_id &eoc : eocs ) {
                    eoc->activate( newDialog );
                }
            }
        }
    };
}

void talk_effect_fun_t::set_weighted_list_eocs( const JsonObject &jo,
        const std::string &member )
{
    weighted_int_list<effect_on_condition_id> eocs;
    for( JsonArray pair : jo.get_array( member ) ) {
        effect_on_condition_id eoc;
        int weight = 1;
        for( JsonValue jv : pair ) {
            if( jv.test_int() ) {
                weight = jv.get_int();
            } else {
                eoc = effect_on_conditions::load_inline_eoc( jv, "" );
            }
        }
        eocs.add( eoc, weight );
    }
    function = [eocs]( const dialogue & ) {
        dialogue d( get_talker_for( get_avatar() ), nullptr );

        effect_on_condition_id eoc = *eocs.pick();
        eoc->activate( d );
    };
}

void talk_effect_fun_t::set_add_morale( const JsonObject &jo, const std::string &member,
                                        bool is_npc )
{
    std::string new_type = jo.get_string( member );
    int_or_var iov_bonus = get_int_or_var( jo, "bonus" );
    int_or_var iov_max_bonus = get_int_or_var( jo, "max_bonus" );
    duration_or_var dov_duration = get_duration_or_var( jo, "duration", false, 1_hours );
    duration_or_var dov_decay_start = get_duration_or_var( jo, "decay_start", false, 30_minutes );
    const bool capped = jo.get_bool( "capped", false );
    function = [is_npc, new_type, iov_bonus, iov_max_bonus, dov_duration, dov_decay_start,
            capped]( const dialogue & d ) {
        d.actor( is_npc )->add_morale( morale_type( new_type ), iov_bonus.evaluate( d.actor( is_npc ) ),
                                       iov_max_bonus.evaluate( d.actor( is_npc ) ), dov_duration.evaluate( d.actor( is_npc ) ),
                                       dov_decay_start.evaluate( d.actor( is_npc ) ),
                                       capped );
    };
}

void talk_effect_fun_t::set_lose_morale( const JsonObject &jo, const std::string &member,
        bool is_npc )
{
    std::string old_morale = jo.get_string( member );
    function = [is_npc, old_morale]( const dialogue & d ) {
        d.actor( is_npc )->remove_morale( morale_type( old_morale ) );
    };
}

void talk_effect_fun_t::set_add_faction_trust( const JsonObject &jo, const std::string &member )
{
    int_or_var trust = get_int_or_var( jo, member );
    function = [trust]( const dialogue & d ) {
        d.actor( true )->get_faction()->trusts_u += trust.evaluate( d.actor( false ) );
    };
}

void talk_effect_fun_t::set_lose_faction_trust( const JsonObject &jo, const std::string &member )
{
    int_or_var trust = get_int_or_var( jo, member );
    function = [trust]( const dialogue & d ) {
        d.actor( true )->get_faction()->trusts_u -= trust.evaluate( d.actor( false ) );
    };
}

void talk_effect_fun_t::set_custom_light_level( const JsonObject &jo, const std::string &member )
{
    int_or_var iov = get_int_or_var( jo, member, true );
    duration_or_var dov_length_min = get_duration_or_var( jo, "length_min", false, 0_seconds );
    duration_or_var dov_length_max = get_duration_or_var( jo, "length_max", false, 0_seconds );
    function = [dov_length_min, dov_length_max, iov]( const dialogue & d ) {
        get_timed_events().add( timed_event_type::CUSTOM_LIGHT_LEVEL,
                                calendar::turn + rng( dov_length_min.evaluate( d.actor( false ) ),
                                        dov_length_max.evaluate( d.actor( false ) ) ) +
                                1_seconds/*We add a second here because this will get ticked on the turn its applied before it has an effect*/,
                                -1, iov.evaluate( d.actor( false ) ) );
    };
}

void talk_effect_fun_t::set_spawn_monster( const JsonObject &jo, const std::string &member,
        bool is_npc )
{
    bool group = jo.get_bool( "group", false );
    mtype_id new_monster;
    mongroup_id group_id;
    if( group ) {
        group_id = mongroup_id( jo.get_string( member ) );
    } else {
        new_monster = mtype_id( jo.get_string( member ) );
    }
    int_or_var iov_target_range = get_int_or_var( jo, "target_range", false, 0 );
    int_or_var iov_hallucination_count = get_int_or_var( jo, "hallucination_count", false, 0 );
    int_or_var iov_real_count = get_int_or_var( jo, "real_count", false, 0 );
    int_or_var iov_min_radius = get_int_or_var( jo, "min_radius", false, 1 );
    int_or_var iov_max_radius = get_int_or_var( jo, "max_radius", false, 10 );

    const bool outdoor_only = jo.get_bool( "outdoor_only", false );

    duration_or_var dov_lifespan_min = get_duration_or_var( jo, "lifespan_min", false, 0_seconds );
    duration_or_var dov_lifespan_max = get_duration_or_var( jo, "lifespan_max", false, 0_seconds );
    cata::optional<std::string> target_var;
    bool global = false;
    if( jo.has_member( "target_var" ) ) {
        JsonObject target_obj = jo.get_object( "target_var" );
        target_var = get_talk_varname( target_obj, "value" );
        global = target_obj.get_bool( "global" );
    }
    function = [is_npc, new_monster, iov_target_range, iov_hallucination_count, iov_real_count,
                        iov_min_radius, iov_max_radius, outdoor_only, group_id, dov_lifespan_min,
            dov_lifespan_max, target_var, global]( const dialogue & d ) {
        monster target_monster;

        if( group_id.is_valid() ) {
            target_monster = monster( MonsterGroupManager::GetRandomMonsterFromGroup( group_id ) );
        } else if( new_monster.is_empty() ) {
            int target_range = iov_target_range.evaluate( d.actor( is_npc ) );
            //grab a random nearby hostile creature to create a hallucination or copy of
            Creature *copy = g->get_creature_if( [target_range]( const Creature & critter ) -> bool {
                bool not_self = get_player_character().pos() != critter.pos();
                bool in_range = std::round( rl_dist_exact( get_player_character().pos(), critter.pos() ) ) <= target_range;
                bool valid_target = get_player_character().attitude_to( critter ) == Creature::Attitude::HOSTILE;
                return not_self && in_range && valid_target;
            } );
            if( copy == nullptr ) {
                return;
            }
            target_monster = *copy->as_monster();
        } else {
            target_monster = monster( new_monster );
        }
        int min_radius = iov_min_radius.evaluate( d.actor( is_npc ) );
        int max_radius = iov_max_radius.evaluate( d.actor( is_npc ) );
        int real_count = iov_real_count.evaluate( d.actor( is_npc ) );
        int hallucination_count = iov_hallucination_count.evaluate( d.actor( is_npc ) );
        cata::optional<time_duration> lifespan;
        talker *target = d.actor( is_npc );
        tripoint target_pos = get_map().getlocal( get_tripoint_from_var( target, target_var, global ) );
        for( int i = 0; i < hallucination_count; i++ ) {
            tripoint spawn_point;
            if( g->find_nearby_spawn_point( target_pos, target_monster.type->id, min_radius,
                                            max_radius, spawn_point, outdoor_only ) ) {
                time_duration min = dov_lifespan_min.evaluate( d.actor( is_npc ) );
                if( min > 0_seconds ) {
                    lifespan = rng( min, dov_lifespan_max.evaluate( d.actor( is_npc ) ) );
                }
                g->spawn_hallucination( spawn_point, target_monster.type->id, lifespan );
            }
        }
        for( int i = 0; i < real_count; i++ ) {
            tripoint spawn_point;
            if( g->find_nearby_spawn_point( target_pos, target_monster.type->id, min_radius,
                                            max_radius, spawn_point, outdoor_only ) ) {
                monster *spawned = g->place_critter_at( target_monster.type->id, spawn_point );
                time_duration min = dov_lifespan_min.evaluate( d.actor( is_npc ) );
                if( min > 0_seconds ) {
                    lifespan = rng( min, dov_lifespan_max.evaluate( d.actor( is_npc ) ) );
                    spawned->set_summon_time( lifespan.value() );
                }
            }
        }
    };
}

void talk_effect_fun_t::set_field( const JsonObject &jo, const std::string &member, bool is_npc )
{
    field_type_str_id new_field = field_type_str_id( jo.get_string( member ) );
    int_or_var iov_intensity = get_int_or_var( jo, "intensity", false, 1 );
    duration_or_var dov_age = get_duration_or_var( jo, "age", false, 1_turns );;
    int_or_var iov_radius = get_int_or_var( jo, "radius", false, 10000000 );

    const bool outdoor_only = jo.get_bool( "outdoor_only", false );
    const bool hit_player = jo.get_bool( "hit_player", true );

    cata::optional<std::string> target_var;
    bool global = false;
    if( jo.has_member( "target_var" ) ) {
        JsonObject target_obj = jo.get_object( "target_var" );
        target_var = get_talk_varname( target_obj, "value" );
        global = target_obj.get_bool( "global", false );
    }

    function = [is_npc, new_field, iov_intensity, dov_age, iov_radius, outdoor_only,
            hit_player, target_var, global]( const dialogue & d ) {
        int radius = iov_radius.evaluate( d.actor( is_npc ) );
        int intensity = iov_intensity.evaluate( d.actor( is_npc ) );

        talker *target = d.actor( is_npc );
        tripoint target_pos = get_tripoint_from_var( target, target_var, global );
        for( const tripoint &dest : get_map().points_in_radius( get_map().getlocal( target_pos ),
                radius ) ) {
            if( !outdoor_only || get_map().is_outside( dest ) ) {
                get_map().add_field( dest, new_field, intensity, dov_age.evaluate( d.actor( is_npc ) ),
                                     hit_player );
            }
        }
    };
}

void talk_effect_t::set_effect_consequence( const talk_effect_fun_t &fun,
        dialogue_consequence con )
{
    effects.push_back( fun );
    guaranteed_consequence = std::max( guaranteed_consequence, con );
}

void talk_effect_t::set_effect_consequence( const std::function<void( npc &p )> &ptr,
        dialogue_consequence con )
{
    talk_effect_fun_t npctalk_setter( ptr );
    set_effect_consequence( npctalk_setter, con );
}

void talk_effect_t::set_effect( const talk_effect_fun_t &fun )
{
    effects.push_back( fun );
    guaranteed_consequence = std::max( guaranteed_consequence, dialogue_consequence::none );
}

void talk_effect_t::set_effect( talkfunction_ptr ptr )
{
    talk_effect_fun_t npctalk_setter( ptr );
    dialogue_consequence response;
    if( ptr == &talk_function::hostile ) {
        response = dialogue_consequence::hostile;
    } else if( ptr == &talk_function::player_weapon_drop ||
               ptr == &talk_function::player_weapon_away ||
               ptr == &talk_function::start_mugging ) {
        response = dialogue_consequence::helpless;
    } else {
        response = dialogue_consequence::none;
    }
    set_effect_consequence( npctalk_setter, response );
}

talk_topic talk_effect_t::apply( dialogue &d ) const
{
    if( d.has_beta ) {
        // Need to get a reference to the mission before effects are applied, because effects can remove the mission
        const mission *miss = d.actor( true )->selected_mission();
        for( const talk_effect_fun_t &effect : effects ) {
            effect( d );
        }
        d.actor( true )->add_opinion( opinion );
        if( miss && ( mission_opinion.trust || mission_opinion.fear ||
                      mission_opinion.value || mission_opinion.anger ) ) {
            const int m_value = d.actor( true )->cash_to_favor( miss->get_value() );
            npc_opinion op;
            op.trust = mission_opinion.trust ? m_value / mission_opinion.trust : 0;
            op.fear = mission_opinion.fear ? m_value / mission_opinion.fear : 0;
            op.value = mission_opinion.value ? m_value / mission_opinion.value : 0;
            op.anger = mission_opinion.anger ? m_value / mission_opinion.anger : 0;
            d.actor( true )->add_opinion( op );
        }
        if( d.actor( true )->turned_hostile() ) {
            d.actor( true )->make_angry();
            return talk_topic( "TALK_DONE" );
        }
    } else {
        for( const talk_effect_fun_t &effect : effects ) {
            effect( d );
        }
    }
    // TODO: this is a hack, it should be in clear_mission or so, but those functions have
    // no access to the dialogue object.
    auto &ma = d.missions_assigned;
    ma.clear();
    if( d.has_beta ) {
        // Update the missions we can talk about (must only be current, non-complete ones)
        for( auto &mission : d.actor( true )->assigned_missions() ) {
            if( mission->get_assigned_player_id() == d.actor( false )->getID() ) {
                ma.push_back( mission );
            }
        }
    }

    return next_topic;
}

talk_effect_t::talk_effect_t( const JsonObject &jo, const std::string &member_name )
{
    load_effect( jo, member_name );
    if( jo.has_object( "topic" ) ) {
        next_topic = load_inline_topic( jo.get_object( "topic" ) );
    } else if( jo.has_string( "topic" ) ) {
        next_topic = talk_topic( jo.get_string( "topic" ) );
    }
}

void talk_effect_t::parse_sub_effect( const JsonObject &jo )
{
    talk_effect_fun_t subeffect_fun;
    const bool is_npc = true;
    if( jo.has_string( "companion_mission" ) ) {
        std::string role_id = jo.get_string( "companion_mission" );
        subeffect_fun.set_companion_mission( role_id );
    } else if( jo.has_string( "u_add_effect" ) ) {
        subeffect_fun.set_add_effect( jo, "u_add_effect" );
    } else if( jo.has_string( "npc_add_effect" ) ) {
        subeffect_fun.set_add_effect( jo, "npc_add_effect", is_npc );
    } else if( jo.has_string( "u_lose_effect" ) ) {
        subeffect_fun.set_remove_effect( jo, "u_lose_effect" );
    } else if( jo.has_string( "npc_lose_effect" ) ) {
        subeffect_fun.set_remove_effect( jo, "npc_lose_effect", is_npc );
    } else if( jo.has_string( "u_add_var" ) ) {
        subeffect_fun.set_add_var( jo, "u_add_var" );
    } else if( jo.has_string( "npc_add_var" ) ) {
        subeffect_fun.set_add_var( jo, "npc_add_var", is_npc );
    } else if( jo.has_string( "u_lose_var" ) ) {
        subeffect_fun.set_remove_var( jo, "u_lose_var" );
    } else if( jo.has_string( "npc_lose_var" ) ) {
        subeffect_fun.set_remove_var( jo, "npc_lose_var", is_npc );
    } else if( jo.has_string( "u_adjust_var" ) ) {
        subeffect_fun.set_adjust_var( jo, "u_adjust_var" );
    } else if( jo.has_string( "npc_adjust_var" ) ) {
        subeffect_fun.set_adjust_var( jo, "npc_adjust_var", is_npc );
    } else if( jo.has_string( "u_add_trait" ) ) {
        subeffect_fun.set_add_trait( jo, "u_add_trait" );
    } else if( jo.has_string( "npc_add_trait" ) ) {
        subeffect_fun.set_add_trait( jo, "npc_add_trait", is_npc );
    } else if( jo.has_string( "u_lose_trait" ) ) {
        subeffect_fun.set_remove_trait( jo, "u_lose_trait" );
    } else if( jo.has_string( "npc_lose_trait" ) ) {
        subeffect_fun.set_remove_trait( jo, "npc_lose_trait", is_npc );
    } else if( jo.has_int( "u_spend_cash" ) ) {
        int cash_change = jo.get_int( "u_spend_cash" );
        subeffect_fun.set_u_spend_cash( cash_change );
    } else if( jo.has_string( "u_sell_item" ) || jo.has_string( "u_buy_item" ) ||
               jo.has_string( "u_spawn_item" ) ||
               jo.has_string( "u_consume_item" ) || jo.has_string( "npc_consume_item" ) ||
               jo.has_string( "u_remove_item_with" ) || jo.has_string( "npc_remove_item_with" ) ) {
        int cost = 0;
        if( jo.has_int( "cost" ) ) {
            cost = jo.get_int( "cost" );
        }
        int count = 1;
        if( jo.has_int( "count" ) ) {
            count = jo.get_int( "count" );
        }
        std::string container_name;
        if( jo.has_string( "container" ) ) {
            container_name = jo.get_string( "container" );
        }
        if( jo.has_string( "u_sell_item" ) ) {
            itype_id item_name;
            jo.read( "u_sell_item", item_name, true );
            subeffect_fun.set_u_sell_item( item_name, cost, count );
        } else if( jo.has_string( "u_buy_item" ) ) {
            if( cost <= 0 ) {
                jo.throw_error( "u_buy_item expecting a non-zero cost parameter", "u_buy_item" );
            }
            itype_id item_name;
            jo.read( "u_buy_item", item_name, true );
            subeffect_fun.set_u_buy_item( item_name, cost, count, container_name );
        } else if( jo.has_string( "u_spawn_item" ) ) {
            itype_id item_name;
            jo.read( "u_spawn_item", item_name, true );
            subeffect_fun.set_u_spawn_item( item_name, count, container_name );
        } else if( jo.has_string( "u_consume_item" ) ) {
            subeffect_fun.set_consume_item( jo, "u_consume_item", count );
        } else if( jo.has_string( "npc_consume_item" ) ) {
            subeffect_fun.set_consume_item( jo, "npc_consume_item", count, is_npc );
        } else if( jo.has_string( "u_remove_item_with" ) ) {
            subeffect_fun.set_remove_item_with( jo, "u_remove_item_with" );
        } else if( jo.has_string( "npc_remove_item_with" ) ) {
            subeffect_fun.set_remove_item_with( jo, "npc_remove_item_with", is_npc );
        }
    } else if( jo.has_int( "u_bulk_trade_accept" ) || jo.has_int( "npc_bulk_trade_accept" ) ||
               jo.has_int( "u_bulk_donate" ) || jo.has_int( "npc_bulk_donate" ) ) {
        talk_effect_fun_t subeffect_fun;
        int quantity = -1;
        bool is_npc = false;
        bool is_trade = false;
        if( jo.has_int( "npc_bulk_trade_accept" ) ) {
            is_npc = true;
            is_trade = true;
            quantity = jo.get_int( "npc_bulk_trade_accept" );
        } else if( jo.has_int( "npc_bulk_donate" ) ) {
            is_npc = true;
            is_trade = false;
            quantity = jo.get_int( "npc_bulk_donate" );
        } else if( jo.has_int( "u_bulk_trade_accept" ) ) {
            is_npc = false;
            is_trade = true;
            quantity = jo.get_int( "u_bulk_trade_accept" );
        } else if( jo.has_int( "u_bulk_donate" ) ) {
            is_npc = false;
            is_trade = false;
            quantity = jo.get_int( "u_bulk_donate" );
        }
        subeffect_fun.set_bulk_trade_accept( is_trade, quantity, is_npc );
        set_effect( subeffect_fun );
        return;
    } else if( jo.has_string( "npc_change_class" ) ) {
        std::string class_name = jo.get_string( "npc_change_class" );
        subeffect_fun.set_npc_change_class( class_name );
    } else if( jo.has_string( "add_mission" ) ) {
        std::string mission_id = jo.get_string( "add_mission" );
        subeffect_fun.set_add_mission( mission_id );
    } else if( jo.has_string( "npc_change_faction" ) ) {
        std::string faction_name = jo.get_string( "npc_change_faction" );
        subeffect_fun.set_npc_change_faction( faction_name );
    } else if( jo.has_int( "u_faction_rep" ) ) {
        int faction_rep = jo.get_int( "u_faction_rep" );
        subeffect_fun.set_change_faction_rep( faction_rep );
    } else if( jo.has_array( "add_debt" ) ) {
        std::vector<trial_mod> debt_modifiers;
        for( JsonArray jmod : jo.get_array( "add_debt" ) ) {
            trial_mod this_modifier;
            this_modifier.first = jmod.next_string();
            this_modifier.second = jmod.next_int();
            debt_modifiers.push_back( this_modifier );
        }
        subeffect_fun.set_add_debt( debt_modifiers );
    } else if( jo.has_string( "toggle_npc_rule" ) ) {
        const std::string rule = jo.get_string( "toggle_npc_rule" );
        subeffect_fun.set_toggle_npc_rule( rule );
    } else if( jo.has_string( "set_npc_rule" ) ) {
        const std::string rule = jo.get_string( "set_npc_rule" );
        subeffect_fun.set_set_npc_rule( rule );
    } else if( jo.has_string( "clear_npc_rule" ) ) {
        const std::string rule = jo.get_string( "clear_npc_rule" );
        subeffect_fun.set_clear_npc_rule( rule );
    } else if( jo.has_string( "set_npc_engagement_rule" ) ) {
        const std::string setting = jo.get_string( "set_npc_engagement_rule" );
        subeffect_fun.set_npc_engagement_rule( setting );
    } else if( jo.has_string( "set_npc_aim_rule" ) ) {
        const std::string setting = jo.get_string( "set_npc_aim_rule" );
        subeffect_fun.set_npc_aim_rule( setting );
    } else if( jo.has_string( "set_npc_cbm_reserve_rule" ) ) {
        const std::string setting = jo.get_string( "set_npc_cbm_reserve_rule" );
        subeffect_fun.set_npc_cbm_reserve_rule( setting );
    } else if( jo.has_string( "set_npc_cbm_recharge_rule" ) ) {
        const std::string setting = jo.get_string( "set_npc_cbm_recharge_rule" );
        subeffect_fun.set_npc_cbm_recharge_rule( setting );
    } else if( jo.has_member( "npc_set_goal" ) ) {
        subeffect_fun.set_npc_goal( jo, "npc_set_goal" );
    } else if( jo.has_member( "mapgen_update" ) ) {
        subeffect_fun.set_mapgen_update( jo, "mapgen_update" );
    } else if( jo.has_object( "u_transform_radius" ) || jo.has_int( "u_transform_radius" ) ) {
        subeffect_fun.set_transform_radius( jo, "u_transform_radius", false );
    } else if( jo.has_object( "npc_transform_radius" ) || jo.has_int( "npc_transform_radius" ) ) {
        subeffect_fun.set_transform_radius( jo, "npc_transform_radius", true );
    } else if( jo.has_object( "u_location_variable" ) ) {
        subeffect_fun.set_location_variable( jo, "u_location_variable", false );
    } else if( jo.has_object( "npc_location_variable" ) ) {
        subeffect_fun.set_location_variable( jo, "npc_location_variable", true );
    } else if( jo.has_string( "u_buy_monster" ) ) {
        const std::string &monster_type_id = jo.get_string( "u_buy_monster" );
        const int cost = jo.get_int( "cost", 0 );
        const int count = jo.get_int( "count", 1 );
        const bool pacified = jo.get_bool( "pacified", false );
        translation name;
        jo.read( "name", name );
        subeffect_fun.set_u_buy_monster( monster_type_id, cost, count, pacified, name );
    } else if( jo.has_string( "u_learn_recipe" ) ) {
        const std::string recipe_id = jo.get_string( "u_learn_recipe" );
        subeffect_fun.set_u_learn_recipe( recipe_id );
    } else if( jo.has_string( "npc_first_topic" ) ) {
        const std::string chat_topic = jo.get_string( "npc_first_topic" );
        subeffect_fun.set_npc_first_topic( chat_topic );
    } else if( jo.has_string( "sound_effect" ) ) {
        subeffect_fun.set_sound_effect( jo, "sound_effect" );
    } else if( jo.has_string( "u_message" ) ) {
        subeffect_fun.set_message( jo, "u_message" );
    } else if( jo.has_string( "npc_message" ) ) {
        subeffect_fun.set_message( jo, "npc_message", true );
    } else if( jo.has_int( "u_add_wet" ) || jo.has_object( "u_add_wet" ) ) {
        subeffect_fun.set_add_wet( jo, "u_add_wet", false );
    } else if( jo.has_int( "npc_add_wet" ) || jo.has_object( "npc_add_wet" ) ) {
        subeffect_fun.set_add_wet( jo, "npc_add_wet", true );
    } else if( jo.has_member( "u_assign_activity" ) ) {
        subeffect_fun.set_assign_activity( jo, "u_assign_activity", false );
    } else if( jo.has_member( "npc_assign_activity" ) ) {
        subeffect_fun.set_assign_activity( jo, "npc_assign_activity", true );
    } else if( jo.has_member( "assign_mission" ) ) {
        subeffect_fun.set_assign_mission( jo, "assign_mission" );
    } else if( jo.has_string( "finish_mission" ) ) {
        subeffect_fun.set_finish_mission( jo, "finish_mission" );
    } else if( jo.has_member( "u_make_sound" ) ) {
        subeffect_fun.set_make_sound( jo, "u_make_sound", false );
    } else if( jo.has_member( "npc_make_sound" ) ) {
        subeffect_fun.set_make_sound( jo, "npc_make_sound", true );
    } else if( jo.has_array( "set_queue_effect_on_condition" ) ) {
        subeffect_fun.set_queue_effect_on_condition( jo, "set_queue_effect_on_condition", false );
    } else if( jo.has_array( "u_set_queue_eoc" ) ) {
        subeffect_fun.set_queue_effect_on_condition( jo, "u_set_queue_eoc", false );
    } else if( jo.has_array( "npc_set_queue_eoc" ) ) {
        subeffect_fun.set_queue_effect_on_condition( jo, "npc_set_queue_eoc", true );
    } else if( jo.has_array( "set_weighted_list_eocs" ) ) {
        subeffect_fun.set_weighted_list_eocs( jo, "set_weighted_list_eocs" );
    } else if( jo.has_member( "u_mod_healthy" ) ) {
        subeffect_fun.set_mod_healthy( jo, "u_mod_healthy", false );
    } else if( jo.has_member( "npc_mod_healthy" ) ) {
        subeffect_fun.set_mod_healthy( jo, "npc_mod_healthy", true );
    } else if( jo.has_string( "u_add_morale" ) ) {
        subeffect_fun.set_add_morale( jo, "u_add_morale", false );
    } else if( jo.has_string( "npc_add_morale" ) ) {
        subeffect_fun.set_add_morale( jo, "npc_add_morale", true );
    } else if( jo.has_string( "u_lose_morale" ) ) {
        subeffect_fun.set_lose_morale( jo, "u_lose_morale", false );
    } else if( jo.has_string( "npc_lose_morale" ) ) {
        subeffect_fun.set_lose_morale( jo, "npc_lose_morale", true );
    } else if( jo.has_int( "u_add_faction_trust" ) || jo.has_object( "u_add_faction_trust" ) ) {
        subeffect_fun.set_add_faction_trust( jo, "u_add_faction_trust" );
    } else if( jo.has_int( "u_lose_faction_trust" ) || jo.has_object( "u_lose_faction_trust" ) ) {
        subeffect_fun.set_lose_faction_trust( jo, "u_lose_faction_trust" );
    } else if( jo.has_string( "u_add_bionic" ) ) {
        subeffect_fun.set_add_bionic( jo, "u_add_bionic", false );
    } else if( jo.has_string( "npc_add_bionic" ) ) {
        subeffect_fun.set_add_bionic( jo, "npc_add_bionic", true );
    } else if( jo.has_string( "u_lose_bionic" ) ) {
        subeffect_fun.set_lose_bionic( jo, "u_lose_bionic", false );
    } else if( jo.has_string( "npc_lose_bionic" ) ) {
        subeffect_fun.set_lose_bionic( jo, "npc_lose_bionic", true );
    } else if( jo.has_member( "u_cast_spell" ) ) {
        subeffect_fun.set_cast_spell( jo, "u_cast_spell", false );
    } else if( jo.has_member( "npc_cast_spell" ) ) {
        subeffect_fun.set_cast_spell( jo, "npc_cast_spell", true );
    } else if( jo.has_array( "arithmetic" ) ) {
        subeffect_fun.set_arithmetic( jo, "arithmetic" );
    } else if( jo.has_string( "u_set_spawn_monster" ) ) {
        subeffect_fun.set_spawn_monster( jo, "u_set_spawn_monster", false );
    } else if( jo.has_string( "npc_set_spawn_monster" ) ) {
        subeffect_fun.set_spawn_monster( jo, "npc_set_spawn_monster", true );
    } else if( jo.has_string( "u_set_field" ) ) {
        subeffect_fun.set_field( jo, "u_set_field", false );
    } else if( jo.has_string( "npc_set_field" ) ) {
        subeffect_fun.set_field( jo, "npc_set_field", true );
    } else if( jo.has_int( "custom_light_level" ) || jo.has_object( "custom_light_level" ) ) {
        subeffect_fun.set_custom_light_level( jo, "custom_light_level" );
    } else {
        jo.throw_error( "invalid sub effect syntax: " + jo.str() );
    }
    set_effect( subeffect_fun );
}

void talk_effect_t::parse_string_effect( const std::string &effect_id, const JsonObject &jo )
{
    static const std::unordered_map<std::string, void( * )( npc & )> static_functions_map = {
        {
#define WRAP( function ) { #function, &talk_function::function }
            WRAP( assign_mission ),
            WRAP( mission_success ),
            WRAP( mission_failure ),
            WRAP( clear_mission ),
            WRAP( mission_reward ),
            WRAP( start_trade ),
            WRAP( sort_loot ),
            WRAP( find_mount ),
            WRAP( dismount ),
            WRAP( do_chop_plank ),
            WRAP( do_vehicle_deconstruct ),
            WRAP( do_vehicle_repair ),
            WRAP( do_chop_trees ),
            WRAP( do_fishing ),
            WRAP( do_construction ),
            WRAP( do_mining ),
            WRAP( do_mopping ),
            WRAP( do_read ),
            WRAP( do_butcher ),
            WRAP( do_farming ),
            WRAP( assign_guard ),
            WRAP( assign_camp ),
            WRAP( abandon_camp ),
            WRAP( stop_guard ),
            WRAP( start_camp ),
            WRAP( buy_cow ),
            WRAP( buy_chicken ),
            WRAP( buy_horse ),
            WRAP( recover_camp ),
            WRAP( remove_overseer ),
            WRAP( basecamp_mission ),
            WRAP( wake_up ),
            WRAP( reveal_stats ),
            WRAP( end_conversation ),
            WRAP( insult_combat ),
            WRAP( give_equipment ),
            WRAP( give_aid ),
            WRAP( give_all_aid ),
            WRAP( barber_beard ),
            WRAP( barber_hair ),
            WRAP( buy_haircut ),
            WRAP( buy_shave ),
            WRAP( morale_chat ),
            WRAP( morale_chat_activity ),
            WRAP( buy_10_logs ),
            WRAP( buy_100_logs ),
            WRAP( bionic_install ),
            WRAP( bionic_remove ),
            WRAP( drop_items_in_place ),
            WRAP( follow ),
            WRAP( follow_only ),
            WRAP( deny_follow ),
            WRAP( deny_lead ),
            WRAP( deny_equipment ),
            WRAP( deny_train ),
            WRAP( deny_personal_info ),
            WRAP( hostile ),
            WRAP( flee ),
            WRAP( leave ),
            WRAP( stop_following ),
            WRAP( revert_activity ),
            WRAP( goto_location ),
            WRAP( stranger_neutral ),
            WRAP( start_mugging ),
            WRAP( player_leaving ),
            WRAP( drop_weapon ),
            WRAP( drop_stolen_item ),
            WRAP( remove_stolen_status ),
            WRAP( player_weapon_away ),
            WRAP( player_weapon_drop ),
            WRAP( lead_to_safety ),
            WRAP( start_training ),
            WRAP( start_training_npc ),
            WRAP( start_training_seminar ),
            WRAP( copy_npc_rules ),
            WRAP( set_npc_pickup ),
            WRAP( npc_die ),
            WRAP( npc_thankful ),
            WRAP( clear_overrides ),
            WRAP( do_disassembly ),
            WRAP( nothing )
#undef WRAP
        }
    };
    const auto iter = static_functions_map.find( effect_id );
    if( iter != static_functions_map.end() ) {
        set_effect( iter->second );
        return;
    }

    talk_effect_fun_t subeffect_fun;
    if( effect_id == "u_bulk_trade_accept" || effect_id == "npc_bulk_trade_accept" ||
        effect_id == "u_bulk_donate" || effect_id == "npc_bulk_donate" ) {
        bool is_npc = effect_id == "npc_bulk_trade_accept" || effect_id == "npc_bulk_donate";
        bool is_trade = effect_id == "u_bulk_trade_accept" || effect_id == "npc_bulk_trade_accept";
        subeffect_fun.set_bulk_trade_accept( is_trade, -1, is_npc );
        set_effect( subeffect_fun );
        return;
    }

    if( effect_id == "lightning" ) {
        subeffect_fun.set_lightning();
        set_effect( subeffect_fun );
        return;
    }

    if( effect_id == "next_weather" ) {
        subeffect_fun.set_next_weather();
        set_effect( subeffect_fun );
        return;
    }

    if( effect_id == "npc_gets_item" || effect_id == "npc_gets_item_to_use" ) {
        bool to_use = effect_id == "npc_gets_item_to_use";
        subeffect_fun.set_npc_gets_item( to_use );
        set_effect( subeffect_fun );
        return;
    }

    if( effect_id == "open_dialogue" ) {
        subeffect_fun.set_open_dialogue();
        set_effect( subeffect_fun );
        return;
    }
    if( effect_id == "take_control" ) {
        subeffect_fun.set_take_control();
        set_effect( subeffect_fun );
        return;
    }
    if( effect_id == "take_control_menu" ) {
        subeffect_fun.set_take_control_menu();
        set_effect( subeffect_fun );
        return;
    }
    jo.throw_error( "unknown effect string", effect_id );
}

void talk_effect_t::load_effect( const JsonObject &jo, const std::string &member_name )
{
    if( jo.has_member( "opinion" ) ) {
        JsonValue jv = jo.get_member( "opinion" );
        // Same format as when saving a game (-:
        opinion.deserialize( jv );
    }
    if( jo.has_member( "mission_opinion" ) ) {
        JsonValue jv = jo.get_member( "mission_opinion" );
        // Same format as when saving a game (-:
        mission_opinion.deserialize( jv );
    }
    if( !jo.has_member( member_name ) ) {
        return;
    } else if( jo.has_string( member_name ) ) {
        const std::string type = jo.get_string( member_name );
        parse_string_effect( type, jo );
    } else if( jo.has_object( member_name ) ) {
        JsonObject sub_effect = jo.get_object( member_name );
        parse_sub_effect( sub_effect );
    } else if( jo.has_array( member_name ) ) {
        for( const JsonValue entry : jo.get_array( member_name ) ) {
            if( entry.test_string() ) {
                const std::string type = entry.get_string();
                parse_string_effect( type, jo );
            } else if( entry.test_object() ) {
                JsonObject sub_effect = entry.get_object();
                parse_sub_effect( sub_effect );
            } else {
                jo.throw_error( "invalid effect array syntax", member_name );
            }
        }
    } else {
        jo.throw_error( "invalid effect syntax", member_name );
    }
}

talk_response::talk_response()
{
    truefalse_condition = []( const dialogue & ) {
        return true;
    };
    mission_selected = nullptr;
    // Why aren't these null ids? Well, it turns out most responses give
    // empty ids, so things like the training code check for these empty ids
    // and when it's given a null id, it breaks
    // FIXME: Use null ids
    skill = skill_id();
    style = matype_id();
    proficiency = proficiency_id();
    dialogue_spell = spell_id();
}

talk_response::talk_response( const JsonObject &jo )
{
    if( jo.has_member( "truefalsetext" ) ) {
        JsonObject truefalse_jo = jo.get_object( "truefalsetext" );
        read_condition<dialogue>( truefalse_jo, "condition", truefalse_condition, true );
        truefalse_jo.read( "true", truetext );
        truefalse_jo.read( "false", falsetext );
    } else {
        jo.read( "text", truetext );
        truefalse_condition = []( const dialogue & ) {
            return true;
        };
    }
    if( jo.has_member( "trial" ) ) {
        JsonObject trial_obj = jo.get_object( "trial" );
        trial = talk_trial( trial_obj );
    }
    if( jo.has_member( "success" ) ) {
        JsonObject success_obj = jo.get_object( "success" );
        success = talk_effect_t( success_obj, "effect" );
    } else if( jo.has_string( "topic" ) ) {
        // This is for simple topic switching without a possible failure
        success.next_topic = talk_topic( jo.get_string( "topic" ) );
        success.load_effect( jo, "effect" );
    } else if( jo.has_object( "topic" ) ) {
        success.next_topic = load_inline_topic( jo.get_object( "topic" ) );
    }
    if( trial && !jo.has_member( "failure" ) ) {
        jo.throw_error( "the failure effect is mandatory if a talk_trial has been defined" );
    }
    if( jo.has_member( "failure" ) ) {
        JsonObject failure_obj = jo.get_object( "failure" );
        failure = talk_effect_t( failure_obj, "effect" );
    }

    // TODO: mission_selected
    // TODO: skill
    // TODO: style
}

json_talk_repeat_response::json_talk_repeat_response( const JsonObject &jo )
{
    if( jo.has_bool( "is_npc" ) ) {
        is_npc = true;
    }
    if( jo.has_bool( "include_containers" ) ) {
        include_containers = true;
    }
    if( jo.has_string( "for_item" ) ) {
        for_item.emplace_back( jo.get_string( "for_item" ) );
    } else if( jo.has_array( "for_item" ) ) {
        for( const std::string line : jo.get_array( "for_item" ) ) {
            for_item.emplace_back( line );
        }
    } else if( jo.has_string( "for_category" ) ) {
        for_category.emplace_back( jo.get_string( "for_category" ) );
    } else if( jo.has_array( "for_category" ) ) {
        for( const std::string line : jo.get_array( "for_category" ) ) {
            for_category.emplace_back( line );
        }
    } else {
        jo.throw_error( "Repeat response with no repeat information!" );
    }
    if( for_item.empty() && for_category.empty() ) {
        jo.throw_error( "Repeat response with empty repeat information!" );
    }
    if( jo.has_object( "response" ) ) {
        JsonObject response_obj = jo.get_object( "response" );
        response = json_talk_response( response_obj );
    } else {
        jo.throw_error( "Repeat response with no response!" );
    }
}

json_talk_response::json_talk_response( const JsonObject &jo )
    : actual_response( jo )
{
    load_condition( jo );
}

void json_talk_response::load_condition( const JsonObject &jo )
{
    has_condition_ = jo.has_member( "condition" );
    is_switch = jo.get_bool( "switch", false );
    is_default = jo.get_bool( "default", false );
    read_condition<dialogue>( jo, "condition", condition, true );
}

bool json_talk_response::test_condition( const dialogue &d ) const
{
    if( condition ) {
        return condition( d );
    }
    return true;
}

const talk_response &json_talk_response::get_actual_response() const
{
    return actual_response;
}

bool json_talk_response::gen_responses( dialogue &d, bool switch_done ) const
{
    if( !is_switch || !switch_done ) {
        if( test_condition( d ) ) {
            d.responses.emplace_back( actual_response );
            return is_switch && !is_default;
        }
    }
    return false;
}

// repeat responses always go in front
bool json_talk_response::gen_repeat_response( dialogue &d, const itype_id &item_id,
        bool switch_done ) const
{
    if( !is_switch || !switch_done ) {
        if( test_condition( d ) ) {
            talk_response result = actual_response;
            result.success.next_topic.item_type = item_id;
            result.failure.next_topic.item_type = item_id;
            d.responses.insert( d.responses.begin(), result );
            return is_switch && !is_default;
        }
    }
    return false;
}

static std::string translate_gendered_line(
    const std::string &line,
    const std::vector<std::string> &relevant_genders,
    const dialogue &d
)
{
    GenderMap gender_map;
    for( const std::string &subject : relevant_genders ) {
        if( subject == "npc" ) {
            gender_map[subject] = d.actor( true )->get_grammatical_genders();
        } else if( subject == "u" ) {
            gender_map[subject] = d.actor( false )->get_grammatical_genders();
        } else {
            debugmsg( "Unsupported subject '%s' for grammatical gender in dialogue", subject );
        }
    }
    return gettext_gendered( gender_map, line );
}

dynamic_line_t dynamic_line_t::from_member( const JsonObject &jo,
        const std::string &member_name )
{
    if( jo.has_array( member_name ) ) {
        return dynamic_line_t( jo.get_array( member_name ) );
    } else if( jo.has_object( member_name ) ) {
        return dynamic_line_t( jo.get_object( member_name ) );
    } else if( jo.has_string( member_name ) ) {
        translation line;
        jo.read( member_name, line );
        return dynamic_line_t( line );
    } else {
        return dynamic_line_t{};
    }
}

dynamic_line_t::dynamic_line_t( const translation &line )
{
    function = [line]( const dialogue & ) {
        return line.translated();
    };
}

dynamic_line_t::dynamic_line_t( const JsonObject &jo )
{
    if( jo.has_member( "and" ) ) {
        std::vector<dynamic_line_t> lines;
        for( const JsonValue entry : jo.get_array( "and" ) ) {
            if( entry.test_string() ) {
                translation line;
                entry.read( line );
                lines.emplace_back( line );
            } else if( entry.test_array() ) {
                lines.emplace_back( entry.get_array() );
            } else if( entry.test_object() ) {
                lines.emplace_back( entry.get_object() );
            } else {
                entry.throw_error( "invalid format: must be string, array or object" );
            }
        }
        function = [lines]( const dialogue & d ) {
            std::string all_lines;
            for( const dynamic_line_t &line : lines ) {
                all_lines += line( d );
            }
            return all_lines;
        };
    } else if( jo.get_bool( "give_hint", false ) ) {
        function = [&]( const dialogue & ) {
            return get_hint();
        };
    } else if( jo.get_bool( "use_reason", false ) ) {
        function = [&]( const dialogue & d ) {
            std::string tmp = d.reason;
            d.reason.clear();
            return tmp;
        };
    } else if( jo.get_bool( "list_faction_camp_sites", false ) ) {
        function = [&]( const dialogue & ) {
            const auto &sites = recipe_group::get_recipes_by_id( "all_faction_base_types", "ANY" );
            if( sites.empty() ) {
                return std::string( _( "I can't think of a single place I can build a camp." ) );
            }
            std::string tmp = "I can start a new camp as a ";
            tmp += enumerate_as_string( sites.begin(), sites.end(),
            []( const std::pair<recipe_id, translation> site ) {
                return site.second.translated();
            },
            enumeration_conjunction::or_ );
            return tmp;
        };
    } else if( jo.has_string( "gendered_line" ) ) {
        std::string line;
        mandatory( jo, false, "gendered_line", line, text_style_check_reader() );
        if( !jo.has_array( "relevant_genders" ) ) {
            jo.throw_error(
                R"(dynamic line with "gendered_line" must also have "relevant_genders")" );
        }
        std::vector<std::string> relevant_genders;
        for( const std::string gender : jo.get_array( "relevant_genders" ) ) {
            relevant_genders.push_back( gender );
            if( gender != "npc" && gender != "u" ) {
                jo.throw_error( "Unexpected subject in relevant_genders; expected 'npc' or 'u'" );
            }
        }
        function = [line, relevant_genders]( const dialogue & d ) {
            return translate_gendered_line( line, relevant_genders, d );
        };
    } else {
        conditional_t<dialogue> dcondition;
        const dynamic_line_t yes = from_member( jo, "yes" );
        const dynamic_line_t no = from_member( jo, "no" );
        for( const std::string &sub_member : dialogue_data::simple_string_conds ) {
            if( jo.has_bool( sub_member ) ) {
                // This also marks the member as visited.
                if( !jo.get_bool( sub_member ) ) {
                    jo.throw_error( "value must be true", sub_member );
                }
                dcondition = conditional_t<dialogue>( sub_member );
                function = [dcondition, yes, no]( const dialogue & d ) {
                    return ( dcondition( d ) ? yes : no )( d );
                };
                return;
            } else if( jo.has_member( sub_member ) ) {
                dcondition = conditional_t<dialogue>( sub_member );
                const dynamic_line_t yes_member = from_member( jo, sub_member );
                function = [dcondition, yes_member, no]( const dialogue & d ) {
                    return ( dcondition( d ) ? yes_member : no )( d );
                };
                return;
            }
        }
        for( const std::string &sub_member : dialogue_data::complex_conds ) {
            if( jo.has_member( sub_member ) ) {
                dcondition = conditional_t<dialogue>( jo );
                function = [dcondition, yes, no]( const dialogue & d ) {
                    return ( dcondition( d ) ? yes : no )( d );
                };
                return;
            }
        }
        jo.throw_error( "dynamic line not supported" );
    }
}

dynamic_line_t::dynamic_line_t( const JsonArray &ja )
{
    std::vector<dynamic_line_t> lines;
    for( const JsonValue entry : ja ) {
        if( entry.test_string() ) {
            translation line;
            entry.read( line );
            lines.emplace_back( line );
        } else if( entry.test_array() ) {
            lines.emplace_back( entry.get_array() );
        } else if( entry.test_object() ) {
            lines.emplace_back( entry.get_object() );
        } else {
            entry.throw_error( "invalid format: must be string, array or object" );
        }
    }
    function = [lines]( const dialogue & d ) {
        const dynamic_line_t &line = random_entry_ref( lines );
        return line( d );
    };
}

json_dynamic_line_effect::json_dynamic_line_effect( const JsonObject &jo,
        const std::string &id )
{
    std::function<bool( const dialogue & )> tmp_condition;
    read_condition<dialogue>( jo, "condition", tmp_condition, true );
    talk_effect_t tmp_effect = talk_effect_t( jo, "effect" );
    // if the topic has a sentinel, it means implicitly add a check for the sentinel value
    // and do not run the effects if it is set.  if it is not set, run the effects and
    // set the sentinel
    if( jo.has_string( "sentinel" ) ) {
        const std::string sentinel = jo.get_string( "sentinel" );
        const std::string varname = "npctalk_var_sentinel_" + id + "_" + sentinel;
        condition = [varname, tmp_condition]( const dialogue & d ) {
            return d.actor( false )->get_value( varname ) != "yes" && tmp_condition( d );
        };
        std::function<void( const dialogue &d )> function = [varname]( const dialogue & d ) {
            d.actor( false )->set_value( varname, "yes" );
        };
        tmp_effect.effects.emplace_back( function );
    } else {
        condition = tmp_condition;
    }
    effect = tmp_effect;
}

bool json_dynamic_line_effect::test_condition( const dialogue &d ) const
{
    return condition( d );
}

void json_dynamic_line_effect::apply( dialogue &d ) const
{
    effect.apply( d );
}

void json_talk_topic::load( const JsonObject &jo )
{
    if( jo.has_member( "dynamic_line" ) ) {
        dynamic_line = dynamic_line_t::from_member( jo, "dynamic_line" );
    }
    if( jo.has_member( "speaker_effect" ) ) {
        std::string id = "no_id";
        if( jo.has_string( "id" ) ) {
            id = jo.get_string( "id" );
        } else if( jo.has_array( "id" ) ) {
            id = jo.get_array( "id" ).next_string();
        }
        if( jo.has_object( "speaker_effect" ) ) {
            JsonObject speaker_effect = jo.get_object( "speaker_effect" );
            speaker_effects.emplace_back( speaker_effect, id );
        } else if( jo.has_array( "speaker_effect" ) ) {
            for( JsonObject speaker_effect : jo.get_array( "speaker_effect" ) ) {
                speaker_effects.emplace_back( speaker_effect, id );
            }
        }
    }
    for( JsonObject response : jo.get_array( "responses" ) ) {
        responses.emplace_back( response );
    }
    if( jo.has_object( "repeat_responses" ) ) {
        repeat_responses.emplace_back( jo.get_object( "repeat_responses" ) );
    } else if( jo.has_array( "repeat_responses" ) ) {
        for( JsonObject elem : jo.get_array( "repeat_responses" ) ) {
            repeat_responses.emplace_back( elem );
        }
    }
    if( responses.empty() ) {
        jo.throw_error( "no responses for talk topic defined", "responses" );
    }
    replace_built_in_responses = jo.get_bool( "replace_built_in_responses",
                                 replace_built_in_responses );
}

bool json_talk_topic::gen_responses( dialogue &d ) const
{
    d.responses.reserve( responses.size() ); // A wild guess, can actually be more or less

    bool switch_done = false;
    for( const json_talk_response &r : responses ) {
        switch_done |= r.gen_responses( d, switch_done );
    }
    for( const json_talk_repeat_response &repeat : repeat_responses ) {
        talker *actor =  d.actor( repeat.is_npc );
        std::function<bool( const item & )> filter = return_true<item>;
        for( const itype_id &item_id : repeat.for_item ) {
            if( actor->charges_of( item_id ) > 0 || actor->has_amount( item_id, 1 ) ) {
                switch_done |= repeat.response.gen_repeat_response( d, item_id, switch_done );
            }
        }
        for( const item_category_id &category_id : repeat.for_category ) {
            const bool include_containers = repeat.include_containers;
            const auto items_with = actor->items_with( [category_id,
            include_containers]( const item & it ) {
                if( include_containers ) {
                    return it.get_category_of_contents().get_id() == category_id;
                }
                return it.type && it.type->category_force == category_id;
            } );
            for( const auto &it : items_with ) {
                switch_done |= repeat.response.gen_repeat_response( d, it->typeId(), switch_done );
            }
        }
    }

    return replace_built_in_responses;
}

cata::flat_set<std::string> json_talk_topic::get_directly_reachable_topics(
    bool only_unconditional ) const
{
    std::vector<std::string> result;

    auto add_reachable_for_response = [&]( const json_talk_response & json_response ) {
        const talk_response &response = json_response.get_actual_response();
        if( !only_unconditional || !json_response.has_condition() ) {
            result.push_back( response.success.next_topic.id );
            result.push_back( response.failure.next_topic.id );
        }
    };

    for( const json_talk_response &r : responses ) {
        add_reachable_for_response( r );
    }
    for( const json_talk_repeat_response &r : repeat_responses ) {
        add_reachable_for_response( r.response );
    }

    return cata::flat_set<std::string>( result.begin(), result.end() );
}

std::string json_talk_topic::get_dynamic_line( const dialogue &d ) const
{
    return dynamic_line( d );
}

std::vector<json_dynamic_line_effect> json_talk_topic::get_speaker_effects() const
{
    return speaker_effects;
}

void json_talk_topic::check_consistency() const
{
    // TODO: check that all referenced topic actually exist. This is currently not possible
    // as they only exist as built in strings, not in the json_talk_topics map.
}

void unload_talk_topics()
{
    json_talk_topics.clear();
}

void load_talk_topic( const JsonObject &jo )
{
    if( jo.has_array( "id" ) ) {
        for( auto &id : jo.get_string_array( "id" ) ) {
            json_talk_topics[id].load( jo );
        }
    } else {
        const std::string id = jo.get_string( "id" );
        json_talk_topics[id].load( jo );
    }
}

std::string npc::pick_talk_topic( const Character &/*u*/ )
{
    if( personality.aggression > 0 ) {
        if( op_of_u.fear * 2 < personality.bravery && personality.altruism < 0 ) {
            set_attitude( NPCATT_MUG );
            return chatbin.talk_mug ;
        }

        if( personality.aggression + personality.bravery - op_of_u.fear > 0 ) {
            return chatbin.talk_stranger_aggressive ;
        }
    }

    if( op_of_u.fear * 2 > personality.altruism + personality.bravery ) {
        return chatbin.talk_stranger_scared;
    }

    if( op_of_u.fear * 2 > personality.bravery + op_of_u.trust ) {
        return chatbin.talk_stranger_wary;
    }

    if( op_of_u.trust - op_of_u.fear +
        ( personality.bravery + personality.altruism ) / 2 > 0 ) {
        return chatbin.talk_stranger_friendly;
    }

    set_attitude( NPCATT_NULL );
    return chatbin.talk_stranger_neutral;
}

bool npc::has_item_whitelist() const
{
    return is_player_ally() && !rules.pickup_whitelist->empty();
}

bool npc::item_name_whitelisted( const std::string &to_match )
{
    if( !has_item_whitelist() ) {
        return true;
    }

    auto &wlist = *rules.pickup_whitelist;
    const rule_state rule = wlist.check_item( to_match );
    if( rule == rule_state::WHITELISTED ) {
        return true;
    }

    if( rule == rule_state::BLACKLISTED ) {
        return false;
    }

    wlist.create_rule( to_match );
    return wlist.check_item( to_match ) == rule_state::WHITELISTED;
}

bool npc::item_whitelisted( const item &it )
{
    if( !has_item_whitelist() ) {
        return true;
    }

    const auto to_match = it.tname( 1, false );
    return item_name_whitelisted( to_match );
}

const json_talk_topic *get_talk_topic( const std::string &id )
{
    auto it = json_talk_topics.find( id );
    if( it == json_talk_topics.end() ) {
        return nullptr;
    }
    return &it->second;
}
