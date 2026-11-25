# A path the program spells out -- a local assigned one literal, the file's
# directory (`__dir__`) joined with literals, `.freeze` -- is known when the
# program is compiled: its require loads that file, where it stands, as the
# literal require of it does. A path built of the
# literals of the file's own layout names the files beside it, and
# loads the one it names when it runs.
name = "computed_known_a"
p require_relative(name)
p require_relative(name)
base = File.join(__dir__, "computed_lit_dir")
p require_relative(base + "/b")
p require_relative("computed_known_c".freeze)
def later = require_relative("#{__dir__}/computed_known_d")
puts "before"
p later
p KNOWN_D
module Layout
  LIBPATH = __dir__ + "/"
  def self.libpath(*a) = File.join(LIBPATH, *a)
end
%w(x y).each { |f| p require(Layout.libpath(["computed_beside", f])) }
p [BESIDE_X, BESIDE_Y]
# a known path naming no file raises where it stands
begin
  require_relative base + "/nope"
rescue LoadError => e
  p [e.class, e.path.end_with?("/computed_lit_dir/nope")]
end
