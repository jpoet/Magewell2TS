// lock_ios.h - Per-stream synchronization.
// Copyright 2016 by David Krauss.
// This source is released under the MIT license, http://opensource.org/licenses/MIT

#ifndef INCLUDED_S6_LOCK_IOS_H
#define INCLUDED_S6_LOCK_IOS_H

#include <ios>
#include <mutex>

namespace s6_lock_ios {

namespace impl {
class manip;
class temporary_lock;
int ios_index();
void init( std::ios_base & s );
}

typedef std::unique_lock< std::recursive_mutex > ios_lock;

// If one thread knows that it is first, it may invoke this setup routine.
template< typename type, typename traits >
std::basic_ios< type, traits > & mutex_init_own( std::basic_ios< type, traits > & s ) {
    struct except_guard {
        std::basic_ios< type, traits > & s;
        std::ios_base::iostate back;
        ~ except_guard() { s.exceptions( back ); }
    } g { s, s.exceptions() };
    /*  Convert all internal errors to ios_base::failure.
        This works around the lack of a status interface in ios_base. */
    s.exceptions( g.back | std::ios_base::badbit );
    
    try {
        impl::init( s );
    } catch (...) {
        s.setstate( std::ios_base::badbit );
    }
    s.tie( nullptr );
    return s;
}

// If no thread is first, each should invoke this before doing anything with the stream.
template< typename type, typename traits >
std::basic_ios< type, traits > & mutex_init( std::basic_ios< type, traits > & s ) {
    static std::mutex critical; // One critical section per specialization. No need to lock dissimilar streams against each other.
    std::lock_guard< std::mutex > guard( critical );
    
    if ( s.pword( impl::ios_index() ) != nullptr ) return s; // Already initialized; need not be in good state.
    return mutex_init_own( s );
}

// I/O manipulator to lock a given stream to a given, scoped object.
impl::manip lock_ios( ios_lock & l );

// I/O manipulator to lock a given stream until the next semicolon.
impl::manip lock_ios( impl::temporary_lock && l /* = {} */ );


namespace impl {

class manip {
    friend manip s6_lock_ios::lock_ios( ios_lock & );
    friend manip s6_lock_ios::lock_ios( impl::temporary_lock && );
    
    ios_lock &l;
    
    manip( ios_lock & in_l ) : l( in_l ) {}
    
    void acquire( std::ios_base & ) const;

    template< typename type, typename traits >
    friend std::basic_istream< type, traits > & operator >> ( std::basic_istream< type, traits > & s, manip const & m ) {
        m.acquire( s );
        return s;
    }
    
    template< typename type, typename traits >
    friend std::basic_ostream< type, traits > & operator << ( std::basic_ostream< type, traits > & s, manip const & m ) {
        m.acquire( s );
        return s;
    }
};

class temporary_lock {
    friend manip s6_lock_ios::lock_ios( impl::temporary_lock && );
    
    ios_lock l;
    
    temporary_lock() = default;
};

}

inline impl::manip lock_ios( ios_lock & l )
    { return { l }; }

inline impl::manip lock_ios( impl::temporary_lock && l = impl::temporary_lock{} )
    { return { l.l }; }

}

#endif
