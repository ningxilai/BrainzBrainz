cmake_minimum_required(VERSION 3.12)

project(musicbrainz)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(include)

# MusicBrainz CLI (type-driven entity registry, fmt=json, popen-curl)
add_executable(musicbrainz src/musicbrainz.cpp)
