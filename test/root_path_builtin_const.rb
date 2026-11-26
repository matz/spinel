# a path from the top level (`::Process::NAME`) names the builtin's constant,
# also inside a namespace that nests a module or class of the same name
module Mine
  module Process
    CLOCK_REALTIME = :mine
  end
  p ::Process::CLOCK_REALTIME == Process::CLOCK_REALTIME
  p Process::CLOCK_REALTIME
end

module App
  class File
    RDONLY = 99
  end
  p ::File::RDONLY
  p File::RDONLY

  module Errno
    class EACCES < StandardError; end
  end

  def self.own
    raise Errno::EACCES, "own"
  rescue Errno::EACCES => e
    e.class
  end
end

begin
  raise ::Errno::EACCES, "x"
rescue SystemCallError => e
  p e.class, e.message
end

begin
  raise ::Errno::EACCES, "y"
rescue Errno::EACCES => e
  p e.class
end

p App.own

# a constant the program writes into a builtin
File::MINE_FLAG = 9
p File::MINE_FLAG, ::File::MINE_FLAG
