# a module written `module File::Constants` inside another namespace reopens
# File::Constants: its constants are read there, not File's (the File::X
# rewrite stays off)
module Outer
  module File::Constants
    MINE = 1
    RDONLY = 77
  end
end
p File::Constants::RDONLY, File::Constants::MINE
