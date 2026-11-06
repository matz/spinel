# An at_exit hook reads the exception ending the program as $!. It is the same
# object from hook to hook, and it keeps what it was raised with: its class, its
# message, its instance variables and its cause.
#
# A hook that raises or exits ends itself, and the hooks after it read what it
# raised. A rescue inside a hook handles its own exception: while its body runs
# $! is that exception, once it is done $! is nil, and the next hook starts
# again from the exception ending the program. A raise inside a hook takes the
# hook's $! as its cause.
class Boom < StandardError
  attr_reader :extra
  def initialize(msg, extra)
    super(msg)
    @extra = extra
  end
end

def describe(label)
  puts "#{label}: #{$!.inspect}"
end

$kept = nil

at_exit do
  puts "last: #{$!.class} #{$!.status} #{$!.message}"
end
at_exit do
  describe "after an exit in a hook"
  exit 4
end
at_exit do
  describe "after a raise in a hook"
  puts "its cause: #{$!.cause.inspect}"
end
at_exit do
  raise "from a hook"
end
at_exit do
  # registered while the hooks run, and run next
  at_exit { puts "registered in a hook: #{$!.inspect}" }
end
at_exit do
  begin
    raise ArgumentError, "handled"
  rescue => e
    puts "in a rescue: #{$!.inspect}, cause #{e.cause.inspect}"
  end
  puts "after that rescue: #{$!.inspect}"
  begin
    raise TypeError, "handled after"
  rescue => e
    puts "and its cause: #{e.cause.inspect}"
  end
  x = (raise "once more" rescue "modifier")
  puts "#{x}: #{$!.inspect}"
end
at_exit do
  begin
    puts "before an ensure: #{$!.inspect}"
  ensure
    puts "in an ensure: #{$!.inspect}"
  end
end
at_exit do
  puts "same object: #{$!.equal?($kept)}"
  describe "from a method"
end
at_exit do
  $kept = $!
  puts "#{$!.class} < StandardError: #{$!.is_a?(StandardError)}"
  puts "message: #{$!.message}"
  puts "extra: #{$!.extra}" if $!.is_a?(Boom)
  puts "cause: #{$!.cause.inspect}"
  puts "backtrace: #{$!.backtrace.class}"
  puts "nil? #{$!.nil?}"
end

puts "body: #{$!.inspect}"
begin
  raise IOError, "first"
rescue
  raise Boom.new("second", 42)
end
