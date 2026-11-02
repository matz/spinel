# As rescue_hook_main.rb, with the hook made by define_method under a
# Symbol: the clause runs, and loads its file.
class RescueHookFoo
  class << self
    define_method(:method_added) { |m| raise LoadError, "hook" if m == :bar }
  end
end
begin
  require_relative "rescue_hook_defs_lib"
rescue LoadError
  require_relative "rescue_hook_fb_lib"
end
p defined?(RescueHookFb)
p RescueHookFb.hi if defined?(RescueHookFb)
