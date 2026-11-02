# A file that only defines things may still raise as it loads, where the
# program defines a definition hook: here method_added raises LoadError for
# the method the file adds, so the rescue clause runs and loads its file.
class RescueHookFoo
  def self.method_added(m)
    raise LoadError, "hook" if m == :bar
  end
end
begin
  require_relative "rescue_hook_defs_lib"
rescue LoadError
  require_relative "rescue_hook_fb_lib"
end
p defined?(RescueHookFb)
p RescueHookFb.hi if defined?(RescueHookFb)
