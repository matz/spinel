# An autoload's file runs where its constant is first reached, a header
# that reopens it too: the class it autoloads has its methods whenever a
# call reaches them, while a method the file defines on another class is
# not there before that.
def err(e = $!) = e.is_a?(NameError) ? [e.class, e.name] : e.class
class RegionAk; end
autoload :RegionAuto, File.join(__dir__, "region_autoload_other_lib")
p((RegionAk.new.m rescue err))
class RegionAuto; def own = :own; end
p RegionAuto.new.lib, RegionAuto.new.own, RegionAuto.k, RegionAk.new.m
