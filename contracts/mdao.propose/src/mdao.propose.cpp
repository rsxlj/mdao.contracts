#include <mdao.propose/mdao.propose.hpp>
#include <mdao.info/mdao.info.db.hpp>
#include <mdao.gov/mdao.gov.hpp>
#include <mdao.treasury/mdao.treasury.hpp>
#include <mdao.stake/mdao.stake.hpp>
#include <thirdparty/utils.hpp>
#include <set>

ACTION mdaoproposal::create(const name& creator, const name& dao_code, const string& title, const string& desc)
{
    require_auth( creator );
    auto conf = _conf();
    CHECKC( conf.status != conf_status::PENDING, gov_err::NOT_AVAILABLE, "under maintenance" );

    governance_t::idx_t governance(MDAO_GOV, MDAO_GOV.value);
    auto gov = governance.find(dao_code.value);
    CHECKC( gov != governance.end(), proposal_err::RECORD_NOT_FOUND, "governance not found" );
    uint64_t vote_strategy_id = gov->strategies.at(strategy_action_type::VOTE);
    uint64_t proposal_strategy_id = gov->strategies.at(strategy_action_type::PROPOSAL);

    strategy_t::idx_t stg(MDAO_STG, MDAO_STG.value);
    auto propose_strategy = stg.find(proposal_strategy_id);
    CHECKC( propose_strategy != stg.end(), proposal_err::RECORD_NOT_FOUND, "strategy not found" );   

    int64_t stg_weight = 0;
    _cal_votes(dao_code, *propose_strategy, creator, stg_weight, 0);
    CHECKC( stg_weight > 0, proposal_err::INSUFFICIENT_BALANCE, "insufficient strategy weight")

    proposal_t::idx_t proposal_tbl(_self, _self.value);
    auto id = _gstate.last_propose_id;
    proposal_tbl.emplace( creator, [&]( auto& row ) {
        row.id                  =   id;
        row.dao_code            =   dao_code;
        row.vote_strategy_id    =   vote_strategy_id;
        row.creator             =   creator;
        row.status              =   proposal_status::CREATED;
        row.desc	            =   desc;
        row.title	            =   title;
        row.proposal_strategy_id=   proposal_strategy_id;
   });
    _gstate.last_propose_id++;
    _global.set( _gstate, get_self() );
}

ACTION mdaoproposal::cancel(const name& owner, const uint64_t& proposal_id)
{
    require_auth( owner );

    auto conf = _conf();
    CHECKC( conf.status != conf_status::PENDING, proposal_err::NOT_AVAILABLE, "under maintenance" );

    proposal_t proposal(proposal_id);
    CHECKC( _db.get(proposal) ,proposal_err::RECORD_NOT_FOUND, "record not found" );
    CHECKC( owner == proposal.creator, proposal_err::PERMISSION_DENIED, "only the creator can operate" );
    CHECKC( proposal_status::CREATED == proposal.status || proposal_status::VOTING == proposal.status, proposal_err::STATUS_ERROR, "can only operate if the state is created and voting" );

    proposal.status  =  proposal_status::CANCELLED;
    _db.set(proposal, _self);
}

ACTION mdaoproposal::addplan( const name& owner, const uint64_t& proposal_id, 
                                const string& title, const string& desc )
{
    require_auth( owner );

    auto conf = _conf();
    CHECKC( conf.status != conf_status::PENDING, proposal_err::NOT_AVAILABLE, "under maintenance" );

    proposal_t proposal(proposal_id);
    CHECKC( _db.get(proposal) ,proposal_err::RECORD_NOT_FOUND, "record not found" );
    CHECKC( owner == proposal.creator, proposal_err::PERMISSION_DENIED, "only the creator can operate" );
    CHECKC( proposal_status::CREATED == proposal.status, proposal_err::STATUS_ERROR, "can only operate if the state is created" );
    
    option p;
    p.title = title;
    p.desc  = desc;
    proposal.options[title] = p;
    _db.set(proposal, _self);
}

ACTION mdaoproposal::startvote(const name& creator, const uint64_t& proposal_id)
{
    require_auth( creator );

    auto conf = _conf();
    CHECKC( conf.status != conf_status::PENDING, proposal_err::NOT_AVAILABLE, "under maintenance" );

    proposal_t proposal(proposal_id);
    CHECKC( _db.get(proposal) ,proposal_err::RECORD_NOT_FOUND, "record not found" );
    CHECKC( proposal.status == proposal_status::CREATED, proposal_err::STATUS_ERROR, "proposal status must be created" );
    CHECKC( creator == proposal.creator, proposal_err::PERMISSION_DENIED, "only the creator can operate" );
    CHECKC( proposal.options.size()>0, proposal_err::PLANS_EMPTY, "please add plan" );

    strategy_t::idx_t stg(MDAO_STG, MDAO_STG.value);
    auto propose_strategy = stg.find(proposal.proposal_strategy_id);

    int64_t stg_weight = 0;
    _cal_votes(proposal.dao_code, *propose_strategy, creator, stg_weight, 0);
    CHECKC( stg_weight > 0, proposal_err::VOTES_NOT_ENOUGH, "insufficient strategy weight")

    proposal.status  =  proposal_status::VOTING;
    proposal.started_at = current_time_point();

    _db.set(proposal, _self);
}

ACTION mdaoproposal::execute( const uint64_t& proposal_id )
{
    auto conf = _conf();
    CHECKC( conf.status != conf_status::PENDING, proposal_err::NOT_AVAILABLE, "under maintenance" );

    proposal_t proposal(proposal_id);
    CHECKC( _db.get(proposal) ,proposal_err::RECORD_NOT_FOUND, "record not found" );
    CHECKC( proposal.status == proposal_status::VOTING, proposal_err::STATUS_ERROR, "proposal status must be running" );

    governance_t::idx_t governance_tbl(MDAO_GOV, MDAO_GOV.value);
    const auto governance = governance_tbl.find(proposal.dao_code.value);
    CHECKC( (proposal.started_at + (governance->voting_period * seconds_per_hour)) >= current_time_point(), proposal_err::ALREADY_EXPIRED, "proposal is already expired" );

    strategy_t::idx_t stg(MDAO_STG, MDAO_STG.value);
    auto vote_strategy = stg.find(proposal.vote_strategy_id);

    switch(vote_strategy->type.value){
        case strategy_type::TOKEN_BALANCE.value:
        case strategy_type::NFT_BALANCE.value:
        case strategy_type::NFT_PARENT_BALANCE.value:
        case strategy_type::TOKEN_SUM.value:{
            CHECKC( proposal.approve_votes >= governance->require_pass, proposal_err::VOTES_NOT_ENOUGH, "votes must meet the minimum number of votes" );
            CHECKC( proposal.approve_votes  > proposal.deny_votes,
                        proposal_err::VOTES_NOT_ENOUGH, "votes must meet the minimum number of votes" );
            break;
        }
        default : {
            mdao::dao_stake_t::idx_t stake(MDAO_STAKE, MDAO_STAKE.value);
            auto stake_itr = stake.find(proposal.dao_code.value);
            CHECKC( proposal.approve_votes >= governance->require_pass, proposal_err::VOTES_NOT_ENOUGH, "votes must meet the minimum number of votes" );
            CHECKC( proposal.users_count >= (governance->require_participation * stake_itr -> user_count / TEN_THOUSAND), proposal_err::VOTES_NOT_ENOUGH, "votes must meet the minimum number of votes" );
        }
    }
    
    if(proposal.options.size() == 1){
        map<string, option>::iterator iter;
        for(iter = proposal.options.begin(); iter != proposal.options.end(); iter++){
            for(action& act : iter->second.execute_actions.actions ) {  
                act.send();
            }   
        }
    }

    proposal.status  =  proposal_status::EXECUTED;
    proposal.executed_at = current_time_point();
    _db.set(proposal, _self);
}

ACTION mdaoproposal::votefor(const name& voter, const uint64_t& proposal_id, 
                                const string& title, const name& vote)
{
    require_auth( voter );

    auto conf = _conf();
    CHECKC( conf.status != conf_status::PENDING, proposal_err::NOT_AVAILABLE, "under maintenance" );

    CHECKC( vote == vote_direction::APPROVE || 
            vote == vote_direction::DENY || 
            vote == vote_direction::WAIVE
            , proposal_err::PARAM_ERROR, "param error" );

    proposal_t proposal(proposal_id);
    CHECKC( _db.get(proposal) ,proposal_err::RECORD_NOT_FOUND, "proposal not found" );
    CHECKC( proposal.status == proposal_status::VOTING, proposal_err::STATUS_ERROR, "proposal status must be running" );

    governance_t::idx_t governance_tbl(MDAO_GOV, MDAO_GOV.value);
    const auto governance = governance_tbl.find(proposal.dao_code.value);
    CHECKC( (proposal.started_at + (governance->voting_period * seconds_per_hour)) >= current_time_point(), proposal_err::ALREADY_EXPIRED, "proposal is already expired" );

    vote_t::idx_t vote_tbl(_self, _self.value);
    auto vote_index = vote_tbl.get_index<"unionid"_n>();
    uint128_t union_id = get_union_id(voter,proposal_id);
    CHECKC( vote_index.find(union_id) == vote_index.end() ,proposal_err::VOTED, "account have voted" );

    strategy_t::idx_t stg(MDAO_STG, MDAO_STG.value);
    auto vote_strategy = stg.find(proposal.vote_strategy_id);

    int64_t stg_weight = 0;
    _cal_votes(proposal.dao_code, *vote_strategy, voter, stg_weight, conf.stake_period_days * seconds_per_day);
    CHECKC( stg_weight > 0, proposal_err::INSUFFICIENT_VOTES, "insufficient votes" );

    auto id = vote_tbl.available_primary_key();
    vote_tbl.emplace( voter, [&]( auto& row ) {
        row.id          =   id;
        row.account     =   voter;
        row.proposal_id =   proposal_id;
        row.direction   =   vote;
        row.vote_weight   =   stg_weight;
        row.voted_at      =   current_time_point();
        row.title         =   title;

    });

    option p = proposal.options[title];
    p.recv_votes = vote == vote_direction::APPROVE ? p.recv_votes + stg_weight : p.recv_votes;

    switch (vote.value)
    {
        case vote_direction::APPROVE.value:{
            proposal.approve_votes += stg_weight;
            break;
        }
        case vote_direction::DENY.value:{
            proposal.deny_votes += stg_weight;
            proposal.deny_users_count++;
            break;
        }
        case  vote_direction::WAIVE.value:{
            proposal.waive_votes += stg_weight;
            proposal.waive_users_count++;
            break;
        }
    }

    proposal.users_count++;
    _db.set(proposal, _self);
}

void mdaoproposal::deletepropose(uint64_t id) {
    proposal_t proposal(id);
    _db.del(proposal);
}
void mdaoproposal::deletevote(uint32_t id) {
    vote_t vote(id);
    vote.id = id;
    _db.del(vote);
}

ACTION mdaoproposal::setaction(const name& owner, const uint64_t& proposal_id,
                                const name& action_name, const name& action_account, 
                                const action_data_variant& data, 
                                const string& title)
{
    require_auth(owner);

    auto conf = _conf();
    CHECKC( conf.status != conf_status::PENDING, proposal_err::NOT_AVAILABLE, "under maintenance" );

    proposal_t proposal(proposal_id);
    CHECKC( _db.get(proposal) ,proposal_err::RECORD_NOT_FOUND, "record not found" );
    CHECKC( owner == proposal.creator, proposal_err::PERMISSION_DENIED, "only the creator can operate" );
    CHECKC( proposal.options.size() == 1, proposal_err::PERMISSION_DENIED, "only type sigle can be used" );

    governance_t::idx_t governance_tbl(MDAO_GOV, MDAO_GOV.value);
    const auto governance = governance_tbl.find(proposal.dao_code.value);
    CHECKC( proposal_status::CREATED == proposal.status, proposal_err::STATUS_ERROR, "can only operate if the state is created" );
    CHECKC(  proposal.options.count(title), proposal_err::PARAM_ERROR, "please add plan" );

    option p = proposal.options[title];
    permission_level pem({_self, "active"_n});

    switch (action_name.value)
    {
        case proposal_action_type::updatedao.value: {
            _check_proposal_params(data, action_name, action_account, proposal.dao_code, conf);
            p.execute_actions.actions.push_back(action(pem, action_account, action_name, data));
            break;
        }
        case proposal_action_type::bindtoken.value: {
            _check_proposal_params(data, action_name, action_account, proposal.dao_code, conf);
            p.execute_actions.actions.push_back(action(pem, action_account, action_name, data));
            break;
        }
        case proposal_action_type::binddapp.value: {
            _check_proposal_params(data, action_name, action_account, proposal.dao_code, conf);
            p.execute_actions.actions.push_back(action(pem, action_account, action_name, data));
            break;
        }
        // case proposal_action_type::createtoken.value: {
        //     createtoken_data action_data = unpack<createtoken_data>(packed_action_data_blob);
        //     action_data_variant data_var = action_data;
        //     _check_proposal_params(data_var, action_name, action_account, proposal.dao_code, conf);
        //     s_plan->execute_actions.actions.push_back(action(pem, action_account, action_name, action_data));
        //     break;
        // }
        // case proposal_action_type::issuetoken.value: {
        //     issuetoken_data action_data = unpack<issuetoken_data>(packed_action_data_blob);
        //     action_data_variant data_var = action_data;
        //     _check_proposal_params(data_var, action_name, action_account, proposal.dao_code, conf);
        //     s_plan->execute_actions.actions.push_back(action(pem, action_account, action_name, action_data));
        //     break;
        // }
        case proposal_action_type::setvotestg.value: {
            _check_proposal_params(data, action_name, action_account, proposal.dao_code, conf);
            p.execute_actions.actions.push_back(action(pem, action_account, action_name, data));
            break;
        }
        case proposal_action_type::setproposestg.value: {
            _check_proposal_params(data, action_name, action_account, proposal.dao_code, conf);
            p.execute_actions.actions.push_back(action(pem, action_account, action_name, data));
            break;
        }
        case proposal_action_type::setlocktime.value: {
            _check_proposal_params(data, action_name, action_account, proposal.dao_code, conf);
            p.execute_actions.actions.push_back(action(pem, action_account, action_name, data));
            break;
        }
         case proposal_action_type::setvotetime.value: {
            _check_proposal_params(data, action_name, action_account, proposal.dao_code, conf);
            p.execute_actions.actions.push_back(action(pem, action_account, action_name, data));
            break;
        }
        // case proposal_action_type::tokentranout.value: {
        //     _check_proposal_params(data, action_name, action_account, proposal.dao_code, conf);
        //     p.execute_actions.actions.push_back(action(pem, action_account, action_name, data));
        //     break;
        // }
        default: {
            CHECKC( false, err::PARAM_ERROR, "Unsupport proposal type")
            break;
        }
    }
    proposal.options[title] = p;
    _db.set(proposal, _self);
}

void mdaoproposal::_check_proposal_params(const action_data_variant& data_var,  const name& action_name, const name& action_account, const name& proposal_dao_code, const conf_t& conf)
{

    switch (action_name.value){
        case proposal_action_type::updatedao.value:{
            updatedao_data data = std::get<updatedao_data>(data_var);
            CHECKC(proposal_dao_code == data.code, proposal_err::PARAM_ERROR, "dao_code error");

            dao_info_t::idx_t info_tbl(MDAO_INFO, MDAO_INFO.value);
            const auto info = info_tbl.find(data.code.value);
            CHECKC(info != info_tbl.end(), proposal_err::RECORD_NOT_FOUND, "record not found");

            CHECKC(info->creator == data.owner, proposal_err::PERMISSION_DENIED, "only the creator can operate");
            CHECKC(!data.groupid.empty(), proposal_err::PARAM_ERROR, "groupid can not be empty");
            CHECKC(data.symcode.empty() == data.symcontract.empty(), proposal_err::PARAM_ERROR, "symcode and symcontract must be null or not null");

            if( !data.symcode.empty() ){
                accounts accountstable(name(data.symcontract), data.owner.value);
                const auto ac = accountstable.find(symbol_code(data.symcode).raw());
                CHECKC(ac != accountstable.end(),  proposal_err::SYMBOL_ERROR, "symcode or symcontract not found");
            }

            break;
        }
        case proposal_action_type::bindtoken.value: {
            bindtoken_data data = std::get<bindtoken_data>(data_var);
            CHECKC(proposal_dao_code == data.code, proposal_err::PARAM_ERROR, "dao_code error");

            dao_info_t::idx_t info_tbl(MDAO_INFO, MDAO_INFO.value);
            const auto info = info_tbl.find(data.code.value);
            CHECKC(info != info_tbl.end(), proposal_err::RECORD_NOT_FOUND, "record not found");
            CHECKC(info->creator == data.owner, proposal_err::PERMISSION_DENIED, "only the creator can operate");

            break;
        }
        case proposal_action_type::binddapp.value: {
            binddapp_data data = std::get<binddapp_data>(data_var);
            CHECKC(proposal_dao_code == data.code, proposal_err::PARAM_ERROR, "dao_code error");

            dao_info_t::idx_t info_tbl(MDAO_INFO, MDAO_INFO.value);
            const auto info = info_tbl.find(data.code.value);
            CHECKC(info != info_tbl.end(), proposal_err::RECORD_NOT_FOUND, "record not found");
            CHECKC(info->creator == data.owner, proposal_err::PERMISSION_DENIED, "only the creator can operate");
            CHECKC( data.dapps.size() != 0 ,proposal_err::CANNOT_ZERO, "dapp size cannot be zero" );

            break;
        }
        // case proposal_action_type::createtoken.value: {
        //     createtoken_data data = std::get<createtoken_data>(data_var);
        //     CHECKC(proposal_dao_code == data.dao_code, proposal_err::PARAM_ERROR, "dao_code error");

        //     CHECKC( data.fullname.size() <= 20, proposal_err::SIZE_TOO_MUCH, "fullname has more than 20 bytes")
        //     CHECKC( data.maximum_supply.amount > 0, proposal_err::NOT_POSITIVE, "not positive quantity:" + data.maximum_supply.to_string() )
        //     symbol_code supply_code = data.maximum_supply.symbol.code();
        //     CHECKC( supply_code.length() > 3, proposal_err::NO_AUTH, "cannot create limited token" )
        //     CHECKC( !conf.black_symbols.count(supply_code) ,proposal_err::NOT_ALLOW, "token not allowed to create" );

        //     stats statstable( MDAO_TOKEN, supply_code.raw() );
        //     CHECKC( statstable.find(supply_code.raw()) == statstable.end(), proposal_err::CODE_REPEAT, "token already exist")

        //     dao_info_t::idx_t info_tbl(MDAO_INFO, MDAO_INFO.value);
        //     const auto info = info_tbl.find(data.code.value);
        //     CHECKC(info != info_tbl.end(), proposal_err::RECORD_NOT_FOUND, "record not found");
        //     CHECKC(info->creator == data.owner, proposal_err::PERMISSION_DENIED, "only the creator can operate");

        //     break;
        // }
        // case proposal_action_type::issuetoken.value: {
        //     issuetoken_data data = std::get<issuetoken_data>(data_var);
        //     CHECKC(proposal_dao_code == data.dao_code, proposal_err::PARAM_ERROR, "dao_code error");

        //     symbol_code supply_code = data.quantity.symbol.code();
        //     stats statstable( MDAO_TOKEN, supply_code.raw() );
        //     CHECKC( statstable.find(supply_code.raw()) != statstable.end(), proposal_err::TOKEN_NOT_EXIST, "token not exist")

        //     dao_info_t::idx_t info_tbl(MDAO_INFO, MDAO_INFO.value);
        //     const auto info = info_tbl.find(data.code.value);
        //     CHECKC(info != info_tbl.end(), proposal_err::RECORD_NOT_FOUND, "record not found");

        //     break;
        // }
        case proposal_action_type::setvotestg.value: {
            setvotestg_data data = std::get<setvotestg_data>(data_var);
            CHECKC(proposal_dao_code == data.dao_code, proposal_err::PARAM_ERROR, "dao_code error");

            governance_t governance(data.dao_code);
            CHECKC( _db.get(governance), proposal_err::RECORD_NOT_FOUND, "governance not exist!" );

            strategy_t::idx_t stg(MDAO_STG, MDAO_STG.value);
            auto vote_strategy = stg.find(data.vote_strategy_id);
            CHECKC( vote_strategy != stg.end(), proposal_err::STRATEGY_NOT_FOUND, "strategy not found" );

            switch (vote_strategy->type.value)
            {
                case strategy_type::NFT_STAKE.value:
                case strategy_type::NFT_PARENT_STAKE.value:
                case strategy_type::TOKEN_STAKE.value:{
                    CHECKC( data.require_participation <= TEN_THOUSAND && data.require_pass <= TEN_THOUSAND, proposal_err::STRATEGY_NOT_FOUND, 
                                "participation no more than" + to_string(TEN_THOUSAND));
                }
                default:
                    break;
            }
            break;
        }
        case proposal_action_type::setproposestg.value:{
            setproposestg_data data = std::get<setproposestg_data>(data_var);
            CHECKC(proposal_dao_code == data.dao_code, proposal_err::PARAM_ERROR, "dao_code error");

            strategy_t::idx_t stg(MDAO_STG, MDAO_STG.value);
            CHECKC(stg.find(data.proposal_strategy_id) != stg.end(), proposal_err::STRATEGY_NOT_FOUND, "strategy not found:"+to_string(data.proposal_strategy_id) );

            break;
        }
        case proposal_action_type::setlocktime.value: {
            setlocktime_data data = std::get<setlocktime_data>(data_var);
            CHECKC(proposal_dao_code == data.dao_code, proposal_err::PARAM_ERROR, "dao_code error");

            governance_t governance(data.dao_code);
            CHECKC( _db.get(governance), proposal_err::RECORD_NOT_FOUND, "governance not exist" );
            CHECKC( data.update_interval >= governance.voting_period, proposal_err::TIME_LESS_THAN_ZERO, "lock time less than vote time" );
            
            break;
        }
         case proposal_action_type::setvotetime.value: {
            setvotetime_data data = std::get<setvotetime_data>(data_var);
            CHECKC(proposal_dao_code == data.dao_code, proposal_err::PARAM_ERROR, "dao_code error");

            governance_t governance(data.dao_code);
            CHECKC( _db.get(governance), proposal_err::RECORD_NOT_FOUND, "governance not exist" );
            break;
        }
        // case proposal_action_type::tokentranout.value: {
        //     tokentranout_data data = std::get<tokentranout_data>(data_var);
        //     CHECKC(proposal_dao_code == data.dao_code, proposal_err::PARAM_ERROR, "dao_code error");
        //     CHECKC( data.quantity.quantity.amount > 0, proposal_err::NOT_POSITIVE, "quantity must be positive" )

        //     treasury_balance_t treasury_balance(data.dao_code);
        //     CHECKC( _db.get(treasury_balance), proposal_err::RECORD_NOT_FOUND, "dao not found" )

        //     uint64_t amount = treasury_balance.stake_assets[data.quantity.get_extended_symbol()];
        //     CHECKC( amount > data.quantity.quantity.amount, proposal_err::INSUFFICIENT_BALANCE, "not sufficient funds " )

        //     break;
        // }
        default:{
            CHECKC(false, err::PARAM_ERROR, "Unsupport proposal type")
            break;
        }
    }
}

void mdaoproposal::recycledb(uint32_t max_rows) {
    require_auth( _self );
    proposal_t::idx_t proposal_tbl(_self, _self.value);
    auto proposal_itr = proposal_tbl.begin();
    for (size_t count = 0; count < max_rows && proposal_itr != proposal_tbl.end(); count++) {
        proposal_itr = proposal_tbl.erase(proposal_itr);
    }
}

void mdaoproposal::_cal_votes(const name dao_code, const strategy_t& vote_strategy, const name voter, int64_t& value, const uint32_t& lock_time) {
    switch(vote_strategy.type.value){
        case strategy_type::TOKEN_STAKE.value : 
        case strategy_type::NFT_STAKE.value : 
        case strategy_type::NFT_PARENT_STAKE.value:{
            value = mdao::strategy::cal_stake_weight(MDAO_STG, vote_strategy.id, dao_code, MDAO_STAKE, voter);

            if(lock_time > 0){
                user_stake_t::idx_t user_stake(MDAO_STAKE, MDAO_STAKE.value); 
                auto user_stake_index = user_stake.get_index<"unionid"_n>(); 
                auto user_stake_iter = user_stake_index.find(mdao::get_unionid(voter, dao_code)); 
                CHECKC( user_stake_iter != user_stake_index.end(), proposal_err::RECORD_NOT_FOUND, "stake record not exist" );

                EXTEND_LOCK(MDAO_STAKE, MDAO_GOV, user_stake_iter->id, lock_time);
            }

            break;
        }
        case strategy_type::TOKEN_BALANCE.value:
        case strategy_type::NFT_BALANCE.value:
        case strategy_type::NFT_PARENT_BALANCE.value:
        case strategy_type::TOKEN_SUM.value: {
            value = mdao::strategy::cal_balance_weight(MDAO_STG, vote_strategy.id, voter);
            break;
         }
        default : {
           CHECKC( false, gov_err::TYPE_ERROR, "type error" );
        }
    }
}

const mdaoproposal::conf_t& mdaoproposal::_conf() {
    if (!_conf_ptr) {
        _conf_tbl_ptr = make_unique<conf_table_t>(MDAO_CONF, MDAO_CONF.value);
        CHECKC(_conf_tbl_ptr->exists(), proposal_err::SYSTEM_ERROR, "conf table not existed in contract" );
        _conf_ptr = make_unique<conf_t>(_conf_tbl_ptr->get());
    }
    return *_conf_ptr;
}

