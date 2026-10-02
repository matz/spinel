# A require in the branch a RUBY_ENGINE check rules out is never resolved,
# just as under `if false` (require_unreachable.rb): the file need not exist,
# and no "not available" warning names it. The analyzer already dropped such
# a branch before compiling it (ruby_engine_dead_branch.rb); the splice runs
# earlier and used to fetch -- or warn about -- the file anyway.
if RUBY_ENGINE == "spinel"
  puts "spinel"
else
  require "spinel_missing_engine_library"
  require_relative "require_unreachable/missing"
end

unless RUBY_ENGINE == "spinel"
  require_relative "require_unreachable/missing"
end

if "jruby" == RUBY_ENGINE
  require_relative "require_unreachable/missing"
end

require "spinel_missing_engine_library" if RUBY_ENGINE != "spinel"
require_relative "require_unreachable/missing" unless RUBY_ENGINE == "spinel"

if !(RUBY_ENGINE == "spinel")
  require_relative "require_unreachable/missing"
end

if RUBY_ENGINE != "spinel"
  $LOAD_PATH << "/nowhere"
  $:.unshift "/nowhere"
  require_relative "require_unreachable/missing"
else
  puts "else"
end

# The comparison is settled only against the engine's own constant: a runtime
# value that happens to hold the name is not folded.
engine = "spinel"
p engine == "spinel"
