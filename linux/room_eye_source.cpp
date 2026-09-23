// Assemble the Vulkan room eye shader, and optionally the plain curved-screen
// shader, from the Windows shader's shared HLSL. Keeping the source in one
// place prevents the ray/finish equations drifting.
//   vrx-room-eye-source xrapp5.cpp room_eye.comp.hlsl [curve.comp.hlsl]
#include "room.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

static std::string Extract(const std::string& source, const char* name) {
    const std::string marker = std::string("static const char* ") + name + " = R\"HLSL(";
    const auto begin = source.find(marker);
    if (begin == std::string::npos) throw std::runtime_error(std::string("Missing shader: ") + name);
    const auto content = begin + marker.size();
    const auto end = source.find(")HLSL\";", content);
    if (end == std::string::npos) throw std::runtime_error(std::string("Unterminated shader: ") + name);
    return source.substr(content, end - content);
}

int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 2;
    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Cannot read Windows shader source");
        const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        const std::string curve = Extract(source, "kCurveHlsl");
        const auto mainStart = curve.find("[numthreads(8, 8, 1)]\nvoid main");
        if (mainStart == std::string::npos) throw std::runtime_error("Curved shader main was not found");
        std::ofstream output(argv[2], std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot write room eye shader");
        output << "#define ROOM_LOOK 1\n#define ROOM_REGISTER b1\n" << RoomHlslDefines()
               << curve.substr(0, mainStart) << Extract(source, "kRoomCbufferHlsl")
               << Extract(source, "kRoomGeomHlsl") << Extract(source, "kCurveRoomHlsl");
        if (!output) throw std::runtime_error("Cannot finish room eye shader");
        if (argc == 4) {
            std::ofstream plain(argv[3], std::ios::binary | std::ios::trunc);
            if (!(plain << curve)) throw std::runtime_error("Cannot write curve shader");
        }
    } catch (const std::exception& error) {
        std::cerr << "Room shader generation: " << error.what() << '\n';
        return 1;
    }
}
