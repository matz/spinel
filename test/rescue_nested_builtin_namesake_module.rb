# Inside a module that nests its own IO::WaitReadable (App::IO::WaitReadable),
# `rescue IO::WaitReadable` matches the program's module, so it rescues a
# class that includes App::IO::WaitReadable. `rescue ::IO::WaitReadable`
# and `is_a?(::IO::WaitReadable)` there take the builtin module.
module App
  module IO
    module WaitReadable; end

    class Err < StandardError
      include WaitReadable
    end
  end

  class BuiltinErr < StandardError
    include ::IO::WaitReadable
  end

  def self.nested(klass)
    raise klass, "x"
  rescue IO::WaitReadable => e
    "#{e.class} rescued as App::IO::WaitReadable"
  rescue StandardError => e
    "#{e.class} missed by App::IO::WaitReadable"
  end

  def self.isa
    [IO::Err.new.is_a?(::IO::WaitReadable), BuiltinErr.new.is_a?(::IO::WaitReadable),
     IO::Err.new.is_a?(IO::WaitReadable), BuiltinErr.new.is_a?(IO::WaitReadable)]
  end

  def self.builtin(klass)
    raise klass, "x"
  rescue ::IO::WaitReadable => e
    "#{e.class} rescued as ::IO::WaitReadable"
  rescue StandardError => e
    "#{e.class} missed by ::IO::WaitReadable"
  end
end

puts App.nested(App::IO::Err)
puts App.nested(App::BuiltinErr)
puts App.builtin(App::IO::Err)
puts App.builtin(App::BuiltinErr)
p App.isa
