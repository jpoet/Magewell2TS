// lock_ios.cpp - Per-stream synchronization.
// Copyright 2016 by David Krauss.
// This source is released under the MIT license, http://opensource.org/licenses/MIT

#include "lock_ios.h"

#include <system_error>

namespace s6_lock_ios {
namespace impl {

int ios_index() {
    static int value = std::ios_base::xalloc();
    return value;
}

void init( std::ios_base & s ) {
    void *& ptr = s.pword( ios_index() );
    if ( ptr ) return;
    
    s.register_callback( + []( std::ios_base::event e, std::ios_base & s, int ) {
        void *& ptr = s.pword( ios_index() );
        if ( e == std::ios_base::erase_event ) {
            // When the stream is terminated, destroy the mutex.
            delete static_cast< std::recursive_mutex * >( ptr );
            ptr = nullptr; // Do not double delete after copyfmt.
            
        } else if ( e == std::ios_base::copyfmt_event ) {
            ptr = nullptr; // Do not copy mutex access.
            // Note, copyfmt does not copy rdbuf or otherwise promote races.
        }
    }, 0 );
    
    ptr = new std::recursive_mutex; // Throw std::bad_alloc or std::system_error.
}

void manip::acquire( std::ios_base & s ) const {
    /*  Accessing pword here assumes that the implementation modifies nothing while
        retrieving a preexisting entry. */
    if ( void * ptr = s.pword( ios_index() ) ) {
        l = ios_lock( * static_cast< std::recursive_mutex * >( ptr ) );
    } else {
        // Imitate error handling of unique_lock::lock.
        throw std::system_error( std::make_error_code( std::errc::operation_not_permitted ), "shared stream is missing a mutex" );
    }
}
}
}
