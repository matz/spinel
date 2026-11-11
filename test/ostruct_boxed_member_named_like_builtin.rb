# An OpenStruct read out of an Array is boxed, and its member named like a
# method of another builtin class (an exception's message, a File's path, an
# IO's read, String#b, Integer#i, Array#pop ...) reads the member, as in
# CRuby. Those names' builtin arms took the OpenStruct for their own
# receiver: they raised NoMethodError, answered nil or 0, or did not build.
# Through the same call site the builtin values beside it still answer as
# their own classes do.
require "ostruct"
require "stringio"

row = OpenStruct.new
%w[message path read value status key result args b i pop shift clear backtrace cause].each_with_index do |n, i|
  row[n] = i
end
r = [row, 1].first
p r.message, r.path, r.read, r.value, r.status, r.key, r.result, r.args
p r.b, r.i, r.pop, r.shift, r.clear, r.backtrace, r.cause

[row, RuntimeError.new("boom")].each { |x| p x.message }
file = File.open(__FILE__) { |f| f }
[row, file].each { |x| p(x.path == __FILE__ ? :this_file : x.path) }
[row, StringIO.new("text")].each { |x| p x.read }
[row, "a"].each { |x| p x.b }
[row, 2].each { |x| p x.i }
[row, [1, 2]].each { |x| p x.pop }
begin
  {}.fetch(:missing)
rescue KeyError => e
  [row, e].each { |x| p x.key }
end

# The same for the Enumerable methods a boxed container answers through
# their builtins/ definitions, the predicates, and the names the analysis
# renames to a builtin's own (grapheme_clusters to chars): the OpenStruct
# reads its member, every other value answers as before.
row2 = OpenStruct.new
%w[max_by tally minmax any? zero? valid? arity grapheme_clusters intern count].each_with_index do |n, i|
  row2[n] = i
end
[row2, [3, 1, 3]].each { |x| p x.tally }
[row2, [3, 1, 2]].each { |x| p x.minmax }
[row2, [3, 1, 2]].each { |x| m = x.max_by; p(m.is_a?(Enumerator) ? :enumerator : m) }
[row2, (1..3)].each { |x| p x.any? }
[row2, 0].each { |x| p x.zero? }
[row2, ->(a) { a }].each { |x| p x.arity }
[row2, "ab"].each { |x| p x.grapheme_clusters }
[row2, "s"].each { |x| p x.intern }
[row2, [1, 2, 2]].each { |x| p x.count }
p [row2, 1].first.valid?
