# A require whose path is computed when it runs and names one of a closed
# set of files -- the words of a literal array a loop goes over -- loads the
# file the path names, when the require runs: once,
# whichever require reaches the file first. A path naming none of them is
# CRuby's LoadError.
order = []
%w(c a).each { |n| order << require_relative("computed_set_#{n}") }
p order
p CSET_A, CSET_C
# the dispatch and a literal require share the file's loaded flag
p require_relative("computed_set_a")
p require_relative("computed_set_b")
p CSET_B
# a word naming no file raises where the require stands
%w(a zzz).each do |n|
  begin
    p require_relative("computed_set_#{n}")
  rescue LoadError => e
    p [e.class, e.path.end_with?("computed_set_zzz")]
  end
end
# a word the loop skips loads nothing
%w(d e).each do |n|
  next if n == "d"
  require_relative "computed_set_#{n}"
end
p defined?(CSET_D), defined?(CSET_E)
puts "done"
