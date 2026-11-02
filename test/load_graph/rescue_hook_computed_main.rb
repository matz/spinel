# As rescue_hook_main.rb, with the hook made by define_method under a name
# the program computes, which its text never spells.
name = %w[method added].join("_")
class RescueHookFoo; end
RescueHookFoo.singleton_class.define_method(name) { |m| raise LoadError, "hook" if m == :bar }
begin
  require_relative "rescue_hook_defs_lib"
rescue LoadError
  require_relative "rescue_hook_fb_lib"
end
p defined?(RescueHookFb)
p RescueHookFb.hi if defined?(RescueHookFb)
