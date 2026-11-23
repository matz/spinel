# With a reopened builtin module (class IO; module WaitReadable) and the
# program's module App::IO::WaitReadable, IO::WaitReadable resolves to the
# builtin and App::IO::WaitReadable resolves to the program's module.
class IO
  module WaitReadable; end
end

module App
  module IO
    module WaitReadable; end
  end
end

p IO::WaitReadable
p App::IO::WaitReadable
p App::IO::WaitReadable == IO::WaitReadable
