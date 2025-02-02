#include "mdao.stg/mdao.stg.hpp"
#include "thirdparty/utils.hpp"

using namespace mdao;
using namespace picomath;

void strategy::create( const name& creator, 
            const string& stg_name, 
            const string& stg_algo,
            const name& type,
            const name& ref_contract,
            const refsymbol& ref_sym){
    require_auth(creator);

    CHECKC( stg_name.size() < MAX_CONTENT_SIZE, err::OVERSIZED, "stg_name length should less than "+ to_string(MAX_CONTENT_SIZE) )
    CHECKC( stg_algo.size() < MAX_ALGO_SIZE, err::OVERSIZED, "stg_algo length should less than "+ to_string(MAX_ALGO_SIZE) )
    
    auto strategies         = strategy_t::idx_t(_self, _self.value);
    auto pid                = strategies.available_primary_key();
    auto strategy           = strategy_t(pid);
    strategy.id             = pid;
    strategy.creator        = creator;
    strategy.stg_name       = stg_name;
    strategy.stg_algo       = stg_algo;
    strategy.type           = type;
    strategy.ref_sym        = ref_sym;
    strategy.ref_contract   = ref_contract;
    strategy.status         = strategy_status::testing;
    strategy.created_at     = current_time_point();

    _db.set( strategy, creator );
}


void strategy::balancestg(const name& creator, 
                const string& stg_name, 
                const uint64_t& balance_value,
                const name& type,
                const name& ref_contract,
                const refsymbol& ref_sym){
    require_auth(creator);
    
    CHECKC( stg_name.size() < MAX_CONTENT_SIZE+1, err::OVERSIZED, "stg_name length should less than "+ to_string(MAX_CONTENT_SIZE) )
    
    string stg_algo = "min(x-"+ to_string(balance_value) + ",1)";

    auto strategies         = strategy_t::idx_t(_self, _self.value);
    auto pid                = strategies.available_primary_key();
    auto strategy           = strategy_t(pid);
    strategy.id             = pid;
    strategy.creator        = creator;
    strategy.stg_name       = stg_name;
    strategy.stg_algo       = stg_algo;
    strategy.type           = type;
    strategy.ref_sym        = ref_sym;
    strategy.ref_contract   = ref_contract;
    strategy.status         = strategy_status::published;
    strategy.created_at     = current_time_point();

    _db.set( strategy, creator );
}

void strategy::setalgo( const name& creator, 
                        const uint64_t& stg_id, 
                        const string& stg_algo ){
    require_auth( creator );

    strategy_t stg = strategy_t(stg_id);
    CHECKC( _db.get( stg ), err::RECORD_NOT_FOUND, "strategy not found: " + to_string(stg_id))
    CHECKC( stg.creator == creator, err::NO_AUTH, "require creator auth")
    CHECKC( stg.status != strategy_status::published, err::NO_AUTH, "cannot monidfy published strategy");

    stg.stg_algo    = stg_algo;
    stg.status      = strategy_status::testing;
    _db.set( stg, creator );
}

void strategy::verify( const name& creator,
                const uint64_t& stg_id, 
                const uint64_t& value,
                const name& account,
                const uint64_t& expect_weight ){
    require_auth( creator );

    CHECKC( is_account(account), err::ACCOUNT_INVALID, "account invalid" )
    CHECKC( expect_weight > 0, err::NOT_POSITIVE, "require positive weight to verify stg" )

    strategy_t stg = strategy_t( stg_id );
    CHECKC( _db.get( stg ), err::RECORD_NOT_FOUND, "strategy not found: " + to_string( stg_id ) )
    CHECKC( stg.status != strategy_status::published, err::NO_AUTH, "cannot verify published strategy" )

    int32_t weight = cal_weight( get_self(), value, stg_id);
    CHECKC( weight == expect_weight, err::UNRESPECT_RESULT, "algo result weight is: "+to_string(weight) )

    stg.status = strategy_status::verified;
    _db.set( stg, creator );
}


void strategy::testalgo( const name& account, const uint64_t& stg_id ){
    strategy_t stg = strategy_t( stg_id );
    CHECKC( _db.get( stg ), err::RECORD_NOT_FOUND, "strategy not found: " + to_string( stg_id ) )

    auto weight = cal_balance_weight(get_self(), stg_id, account);
    check(false, "weight: "+ to_string(weight));
}

void strategy::remove( const name& creator, 
                       const uint64_t& stg_id ){
    require_auth( creator );

    strategy_t stg = strategy_t( stg_id );
    CHECKC( _db.get( stg ), err::RECORD_NOT_FOUND, "strategy not found: " + to_string(stg_id) )
    CHECKC( stg.creator == creator, err::NO_AUTH, "require creator auth" )
    CHECKC( stg.status != strategy_status::published, err::NO_AUTH, "cannot remove published strategy" );

    _db.del( stg );
}

void strategy::publish( const name& creator, 
                        const uint64_t& stg_id ){
    require_auth( creator );

    strategy_t stg = strategy_t( stg_id );
    CHECKC( _db.get( stg ), err::RECORD_NOT_FOUND, "strategy not found: " + to_string( stg_id ) )
    CHECKC( stg.creator == creator, err::NO_AUTH, "require creator auth" )
    CHECKC( stg.status == strategy_status::verified, err::NO_AUTH, "please verify your strategy before publish" );

    stg.status = strategy_status::published;
    _db.set( stg, creator );
}
