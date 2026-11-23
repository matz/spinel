# The program's modules App::IO, App::Process, App::Errno and A::B::IO have
# their own members. App::IO::WaitReadable, and IO::WaitReadable written
# inside App, resolve to the program's module, and ::IO::WaitReadable
# resolves to the builtin.
module App
  module IO
    module WaitReadable; end
  end

  module Process
    class Status; end
  end

  module Errno
    class ENOENT < StandardError; end
  end

  def self.inside
    [IO::WaitReadable, Process::Status, ::IO::WaitReadable, Errno::ENOENT]
  end
end

module A
  module B
    module IO
      module WaitWritable; end
    end

    def self.inside
      IO::WaitWritable
    end
  end
end

p App::IO::WaitReadable
p App::IO::WaitReadable.name
p App::IO::WaitReadable == IO::WaitReadable
p App::Process::Status
p App.inside
p IO::WaitReadable
p App::Errno::ENOENT
p Errno::ENOENT
p A::B::IO::WaitWritable
p A::B.inside
