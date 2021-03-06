# ***************************************************************************
# *   Copyright 2015 Michael Eischer, Philipp Nordhus                       *
# *   Robotics Erlangen e.V.                                                *
# *   http://www.robotics-erlangen.de/                                      *
# *   info@robotics-erlangen.de                                             *
# *                                                                         *
# *   This program is free software: you can redistribute it and/or modify  *
# *   it under the terms of the GNU General Public License as published by  *
# *   the Free Software Foundation, either version 3 of the License, or     *
# *   any later version.                                                    *
# *                                                                         *
# *   This program is distributed in the hope that it will be useful,       *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU General Public License for more details.                          *
# *                                                                         *
# *   You should have received a copy of the GNU General Public License     *
# *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
# ***************************************************************************

add_subdirectory(processor)
add_subdirectory(simulator)
add_subdirectory(strategy)

if(TARGET processor AND TARGET simulator AND TARGET strategy)

include_directories(${PROTOBUF_INCLUDE_DIR})

set(SOURCES
    amun.cpp
    amun.h
    networkinterfacewatcher.cpp
    networkinterfacewatcher.h
    receiver.cpp
    receiver.h
)

add_library(amun ${SOURCES})
target_link_libraries(amun processor simulator strategy)
qt5_use_modules(amun Core Network)

if(WIN32)
    target_link_libraries(amun wsock32)
endif()

endif()
