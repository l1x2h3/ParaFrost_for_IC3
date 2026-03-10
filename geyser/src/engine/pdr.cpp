#include "pdr.hpp"
#include "logger.hpp"
#include <queue>
#include <ranges>
#include <string>

// Include ParaFROST headers for preprocessing (placeholder)
// #include "../../ParaFROST/src/cpu/solver.h"

// Include standard libraries for file I/O
#include <fstream>
#include <filesystem>

namespace geyser::pdr
{

result pdr::run( const transition_system& system )
{
    _system = &system;
    initialize();

    return check();
}

void pdr::initialize()
{
    // Apply GPU-accelerated preprocessing if enabled
    if (_use_gpu_preprocessing) {
        apply_gpu_preprocessing();
    }

    push_frame();

    // We assume that initial states are given as a single cube. This is the
    // case when no invariant constraints are present in the Aiger input, which
    // we assume for the sake of simplicity of implementation.

    _init_cube = formula_as_cube( _system->init() );


    const auto activated_init = _system->init().activate( _trace_activators[ 0 ].var() );

    _basic_solver.assert_formula( activated_init );

    _trans_solver.assert_formula( activated_init );
    _trans_solver.assert_formula( _system->trans() );

    _error_solver.assert_formula( activated_init );
    _error_solver.assert_formula( _system->error() );
}

void pdr::refresh_solvers()
{
    logger::log_line_debug( "Refreshing solvers at depth {}", depth() );

    // Recreate solvers with fresh state
    _basic_solver = solver{};
    _trans_solver = solver{};
    _error_solver = solver{};

    // Re-assert base formulas
    const auto activated_init = _system->init().activate( _trace_activators[ 0 ].var() );

    _basic_solver.assert_formula( activated_init );

    _trans_solver.assert_formula( activated_init );
    _trans_solver.assert_formula( _system->trans() );

    _error_solver.assert_formula( activated_init );
    _error_solver.assert_formula( _system->error() );

    // Re-assert all blocked cubes
    for ( int level = 1; level <= depth(); ++level )
    {
        for ( const auto& cube : _trace_blocked_cubes[ level ] )
        {
            const auto activated = cube.negate().activate( _trace_activators[ level ].var() );

            _basic_solver.assert_formula( activated );
            _trans_solver.assert_formula( activated );
            _error_solver.assert_formula( activated );
        }
    }

    _solver_refresh_counter = 0;
}

void pdr::apply_gpu_preprocessing()
{
    std::cout << "DEBUG: Starting GPU preprocessing" << std::endl;
    logger::log_line_debug( "Applying GPU-accelerated preprocessing" );

    // Extract CNF clauses from transition system formulas
    auto init_clauses = extract_clauses(_system->init());
    auto trans_clauses = extract_clauses(_system->trans());
    auto error_clauses = extract_clauses(_system->error());
    
    std::cout << "DEBUG: init_clauses size: " << init_clauses.size() << std::endl;
    std::cout << "DEBUG: trans_clauses size: " << trans_clauses.size() << std::endl;
    std::cout << "DEBUG: error_clauses size: " << error_clauses.size() << std::endl;

    // Create temporary DIMACS file
    std::string temp_file = "/tmp/ic3_preprocess.cnf";
    write_dimacs_file(temp_file, init_clauses, trans_clauses, error_clauses);

    // =====================================================================
    // NOTE: currently we do *not* invoke a dedicated preprocessing API from
    // ParaFROST.  Instead the implementation simply calls the full solver
    // binary with a temporary CNF file.  This works as a placeholder but
    // is sub‑optimal: we pay solver startup overhead and cannot easily pass
    // incremental clause sets.
    //
    // TODO: replace this hack with a proper library/API call that exposes
    // only the preprocessing routines (BVE, SUB, ERE, BCE, etc.).  With a
    // real API we can also support incremental invocation as IC3 generates
    // new blocked cubes.
    // =====================================================================

    // Call ParaFROST GPU preprocessing
    // Use GPU version if available, otherwise CPU
    std::string parafrost_path;
    std::string gpu_path = "/root/ParaFrost_for_IC3/build/gpu/bin/parafrost";
    std::string cpu_path = "/root/ParaFrost_for_IC3/build/cpu/bin/parafrost";
    
    if (std::filesystem::exists(gpu_path)) {
        parafrost_path = gpu_path;
        std::cout << "DEBUG: Found GPU ParaFROST at " << parafrost_path << std::endl;
        logger::log_line_debug( "Using ParaFROST GPU version for preprocessing" );
    } else if (std::filesystem::exists(cpu_path)) {
        parafrost_path = cpu_path;
        std::cout << "DEBUG: Found CPU ParaFROST at " << parafrost_path << std::endl;
        logger::log_line_debug( "Using ParaFROST CPU version for preprocessing" );
    } else {
        std::cout << "DEBUG: ParaFROST not found at " << cpu_path << ", skipping preprocessing" << std::endl;
        logger::log_line_debug( "ParaFROST not found, skipping preprocessing" );
        std::filesystem::remove(temp_file);
        return;
    }

    // Run ParaFROST with preprocessing options
    // ParaFROST performs preprocessing by default, let's try without -p
    std::string command = parafrost_path + " " + temp_file + " > /tmp/preprocess_output.txt 2>&1";
    std::cout << "DEBUG: Running command: " << command << std::endl;
    int result = std::system(command.c_str());
    std::cout << "DEBUG: ParaFROST result: " << result << std::endl;

    if (result == 0) {
        // Check if preprocessing output file exists (ParaFROST may create simplified CNF)
        std::string simplified_file = temp_file + ".simplified";
        if (std::filesystem::exists(simplified_file)) {
            // TODO: Parse simplified CNF and update _system formulas
            // For now, just log success
            logger::log_line_debug( "Preprocessing completed successfully, simplified formula available" );
        } else {
            logger::log_line_debug( "Preprocessing completed but no simplified output found" );
        }
    } else {
        logger::log_line_debug( "Preprocessing failed, using original formulas" );
    }

    // Clean up
    std::filesystem::remove(temp_file);

    logger::log_line_debug( "GPU preprocessing framework completed" );
}

void pdr::write_dimacs_file(const std::string& filename,
                           const std::vector<std::vector<int>>& init_clauses,
                           const std::vector<std::vector<int>>& trans_clauses,
                           const std::vector<std::vector<int>>& error_clauses)
{
    // Combine all clauses
    std::vector<std::vector<int>> all_clauses;
    all_clauses.insert(all_clauses.end(), init_clauses.begin(), init_clauses.end());
    all_clauses.insert(all_clauses.end(), trans_clauses.begin(), trans_clauses.end());
    all_clauses.insert(all_clauses.end(), error_clauses.begin(), error_clauses.end());

    // Find max variable
    int max_var = 0;
    for (const auto& clause : all_clauses) {
        for (int lit : clause) {
            max_var = std::max(max_var, std::abs(lit));
        }
    }
    
    // Ensure at least one variable for ParaFROST
    if (max_var == 0) {
        max_var = 1;
        // Add a dummy clause: 1 0 (variable 1 is true)
        all_clauses.push_back({1});
    }

    // Write DIMACS format
    std::ofstream file(filename);
    file << "p cnf " << max_var << " " << all_clauses.size() << "\n";

    for (const auto& clause : all_clauses) {
        for (int lit : clause) {
            file << lit << " ";
        }
        file << "0\n";
    }

    file.close();
}

std::vector<std::vector<int>> pdr::extract_clauses(const cnf_formula& formula)
{
    std::vector<std::vector<int>> clauses;
    std::vector<int> current_clause;

    for (const auto& lit : formula.literals()) {
        if (lit == literal::separator) {
            if (!current_clause.empty()) {
                clauses.push_back(std::move(current_clause));
                current_clause.clear();
            }
        } else {
            // Convert literal to DIMACS format (ParaFROST expects int)
            int dimacs_lit = lit.sign() ? lit.value() : -lit.value();
            current_clause.push_back(dimacs_lit);
        }
    }

    return clauses;
}

result pdr::check()
{
    while ( true )
    {
        if ( const auto cti = get_error_cti(); cti.has_value() )
        {
            if ( const auto cex = solve_obligation( proof_obligation{ *cti, depth() } ); cex.has_value() )
                return *cex;
        }
        else
        {
            push_frame();

            if ( propagate() )
                return ok{};
        }

        _ctis.flush();

        // Refresh solvers periodically to prevent bloat
        _solver_refresh_counter++;
        if ( _solver_refresh_counter >= SOLVER_REFRESH_RATE )
        {
            refresh_solvers();
        }
    }
}

// Returns a cti handle to a state which reaches error under given
// input variable values.
std::optional< cti_handle > pdr::get_error_cti()
{
    if ( _error_solver.query()
         .assume( activators_from( depth() ) )
         .is_sat() )
        return _ctis.make( cube{ _error_solver.get_model( _system->state_vars() ) },
                           cube{ _error_solver.get_model( _system->input_vars() ) } );

    return {};
}

std::optional< counterexample > pdr::solve_obligation( const proof_obligation& starting_po )
{
    assert( 0 <= starting_po.level() && starting_po.level() <= depth() );

    auto min_queue = std::priority_queue< proof_obligation,
        std::vector< proof_obligation >, std::greater<> >{};

    min_queue.push( starting_po );

    while ( !min_queue.empty() )
    {
        auto po = min_queue.top();
        min_queue.pop();

        if ( po.level() == 0 )
            return build_counterexample( po.handle() );

        if ( is_already_blocked( po ) )
            continue;

        assert( !intersects_initial_states( _ctis.get( po.handle() ).state_vars().literals() ) );

        if ( is_relative_inductive( _ctis.get( po.handle() ).state_vars().literals(), po.level() ) )
        {
            const auto [ c, i ] = generalize_inductive( po );

            logger::log_line_debug( "{}: {}", i, cube_to_string( c ) );
            add_blocked_at( c, i );

            if ( po.level() < depth() )
                min_queue.emplace( po.handle(), po.level() + 1 );
        }
        else
        {
            min_queue.emplace( get_predecessor( po ), po.level() - 1 );
            min_queue.push( po );
        }
    }

    return {};
}

// Fetch a generalized predecessor of a proof obligation po from a model of
// the last relative inductive check.
cti_handle pdr::get_predecessor( const proof_obligation& po )
{
    const auto& s = _ctis.get( po.handle() ).state_vars().literals();
    auto ins = _trans_solver.get_model( _system->input_vars() );
    auto p = _trans_solver.get_model( _system->state_vars() );

    [[maybe_unused]]
    const auto sat = _trans_solver.query()
            .constrain_not_mapped( s, [ & ]( literal l ){ return _system->prime( l ); } )
            .assume( ins )
            .assume( p )
            .is_sat();

    assert( !sat );

    return _ctis.make( cube{ _trans_solver.get_core( p ) }, cube{ std::move( ins ) }, po.handle() );
}

std::pair< std::vector< literal >, int > pdr::generalize_from_core( std::span< const literal > s, int level )
{
    int j = depth();

    for ( int i = level - 1; i <= depth(); ++i )
    {
        if ( _trans_solver.is_in_core( _trace_activators[ i ] ) )
        {
            j = i;
            break;
        }
    }

    auto res_lits = std::vector< literal >{};

    for ( const auto lit : s )
        if ( _trans_solver.is_in_core( _system->prime( lit ) ) )
            res_lits.emplace_back( lit );

    if ( intersects_initial_states( res_lits ) )
    {
        for ( const auto lit : s )
        {
            if ( _init_cube.contains( !lit ) )
            {
                res_lits.emplace_back( lit );
                break;
            }
        }
    }

    assert( !intersects_initial_states( res_lits ) );

    return { std::move( res_lits ), j + 1 };
}

// Proof obligation po was blocked, i.e. it has no predecessors at the previous
// level. Its cube is therefore inductive relative to the previous level. Try
// to shrink it and possibly move it further along the trace.
std::pair< cube, int > pdr::generalize_inductive( const proof_obligation& po )
{
    auto [ res_lits, res_level ] = generalize_from_core( _ctis.get( po.handle() ).state_vars().literals(), po.level() );
    const auto all_lits = res_lits;

    for ( const auto lit : all_lits )
    {
        const auto it = std::remove( res_lits.begin(), res_lits.end(), lit );

        if ( it == res_lits.end() )
            continue;

        res_lits.erase( it, res_lits.end() );

        if ( intersects_initial_states( res_lits ) || !is_relative_inductive( res_lits, res_level ) )
            res_lits.emplace_back( lit );
        else
            std::tie( res_lits, res_level ) = generalize_from_core( res_lits, res_level );
    }

    while ( res_level <= depth() )
    {
        if ( is_relative_inductive( res_lits, res_level + 1 ) )
            std::tie( res_lits, res_level ) = generalize_from_core( res_lits, res_level + 1 );
        else
            break;
    }

    return { cube{ std::move( res_lits ) }, res_level };
}

counterexample pdr::build_counterexample( cti_handle initial )
{
    logger::log_line_loud( "Found a counterexample at k = {}", depth() );

    // CTI entries don't necessarily contain all the variables. If a variable
    // doesn't appear in any literal, its value is not important, so we might
    // as well just make it false.
    auto get_vars = []( variable_range range, const cube& val )
    {
        auto row = valuation{};
        row.reserve( range.size() );

        for ( const auto var : range )
            row.push_back( val.find( var ).value_or( literal{ var, true } ) );

        return row;
    };

    auto entry = std::optional{ _ctis.get( initial ) };

    auto initial_state = get_vars( _system->state_vars(), entry->state_vars() );

    auto inputs = std::vector< valuation >{};
    inputs.reserve( depth() );

    while ( entry.has_value() )
    {
        inputs.emplace_back( get_vars( _system->input_vars(), entry->input_vars() ) );
        entry = entry->successor().transform( [ & ]( cti_handle h ){ return _ctis.get( h ); } );
    }

    return counterexample{ std::move( initial_state ), std::move( inputs ) };
}

bool pdr::is_already_blocked( const proof_obligation& po )
{
    assert( 1 <= po.level() );

    if ( po.level() > depth() )
        return false;

    const auto& s = _ctis.get( po.handle() ).state_vars();

    for ( const auto& frame : frames_from( po.level() ) )
        for ( const auto& cube : frame )
            if ( cube.subsumes( s ) )
                return true;

    return !_basic_solver.query()
            .assume( s.literals() )
            .assume( activators_from( po.level() ) )
            .is_sat();
}

bool pdr::intersects_initial_states( std::span< const literal > c )
{
    for ( const auto lit : c )
        if ( _init_cube.contains( !lit ) )
            return false;

    return true;
}

// Check whether a cube s in R_i is inductive relative to R_{i - 1}, i.e.
// whether the formula R_{i - 1} /\ -s /\ T /\ s' is unsatisfiable.
bool pdr::is_relative_inductive( std::span< const literal > s, int i )
{
    assert( i >= 1 );

    return !_trans_solver.query()
            .constrain_not( s )
            .assume( activators_from( i - 1 ) )
            .assume_mapped( s, [ & ]( literal l ){ return _system->prime( l ); } )
            .is_sat();
}

void pdr::add_blocked_at( const cube& c, int level, int start_from /* = 1*/ )
{
    assert( 1 <= level );
    assert( 1 <= start_from && start_from <= level );
    assert( is_state_cube( c.literals() ) );

    const auto k = std::min( level, depth() );

    for ( int d = start_from; d <= k; ++d )
    {
        auto& cubes = _trace_blocked_cubes[ d ];

        for ( std::size_t i = 0; i < cubes.size(); )
        {
            if ( c.subsumes( cubes[ i ] ) )
            {
                cubes[ i ] = cubes.back();
                cubes.pop_back();
            }
            else
                ++i;
        }
    }

    assert( k < _trace_blocked_cubes.size() );
    assert( k < _trace_activators.size() );

    _trace_blocked_cubes[ k ].emplace_back( c );

    const auto activated = c.negate().activate( _trace_activators[ k ].var() );

    _basic_solver.assert_formula( activated );
    _trans_solver.assert_formula( activated );
    _error_solver.assert_formula( activated );
}

// Returns true if the system has been proven safe by finding an invariant.
bool pdr::propagate()
{
    assert( _trace_blocked_cubes[ depth() ].empty() );

    for ( int i = 1; i < depth(); ++i )
    {
        // The copy is done since the _trace_blocked_cubes[ i ] will be changed
        // during the forthcoming iteration.
        const auto cubes = _trace_blocked_cubes[ i ];

        for ( const auto& c : cubes )
        {
            if ( is_relative_inductive( c.literals(), i + 1 ) )
            {
                const auto [ lits, level ] = generalize_from_core( c.literals(), i + 1 );
                add_blocked_at( cube{ lits }, level, i );
            }
        }

        if ( _trace_blocked_cubes[ i ].empty() )
            return true;
    }

    log_trace_content();

    return false;
}

// Returns true if cube contains only state variables. Used for assertions
// only.
bool pdr::is_state_cube( std::span< const literal > literals ) const
{
    const auto is_state_var = [ & ]( variable var )
    {
        const auto [ type, _ ] = _system->get_var_info( var );
        return type == var_type::state;
    };

    return std::ranges::all_of( literals, [ & ]( literal lit ){ return is_state_var( lit.var() ); } );
}

void pdr::log_trace_content() const
{
    auto line = std::format( "{}:", depth() );

    for ( int i = 1; i <= depth(); ++i )
        line += std::format( " {}", _trace_blocked_cubes[ i ].size() );

    logger::log_line_loud( "{}", line );
}

} // namespace geyser::pdr