# A program that assigns its own IO constant reads App::IO::X from that
# constant, and File::Constants::X from File.
module Flags; NULL = "flags-null"; RDONLY = 7; end
module App
  IO = Flags
end
p App::IO::NULL
p App::IO::RDONLY
p File::Constants::RDONLY
p File::Constants::LOCK_EX
