# A raise of a class with no message reads its class name as its message, and
# NameError reads the name, as a rescue would.
class Stopped < StandardError; end
at_exit { puts "#{$!.class} #{$!.message} #{$!.inspect}" }
raise Stopped
