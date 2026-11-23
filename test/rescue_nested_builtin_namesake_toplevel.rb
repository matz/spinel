# Inside App, where App::IO::WaitReadable = ::IO::WaitReadable,
# `rescue IO::WaitReadable` resolves to the builtin module and matches
# App::BuiltinErr. A raised ::WaitReadable matches `rescue ::WaitReadable`.
class WaitReadable < StandardError; end

module App
  module IO
    WaitReadable = ::IO::WaitReadable
  end

  class BuiltinErr < StandardError
    include ::IO::WaitReadable
  end

  def self.go(klass)
    raise klass, "x"
  rescue IO::WaitReadable => e
    "#{e.class} rescued as IO::WaitReadable"
  rescue ::WaitReadable => e
    "#{e.class} rescued as ::WaitReadable"
  end
end

puts App.go(::WaitReadable)
puts App.go(App::BuiltinErr)
