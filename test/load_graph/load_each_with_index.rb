# A computed require in `%w(...).each_with_index { |word, i| ... }` names one
# file for each word, as the same loop with `each` does (--load-model=graph):
# the first block parameter is the word, the second the index.
$LOAD_PATH.unshift __dir__
%w(a b).each_with_index do |v, i|
  require File.join("load_each_with_index", v)
end
p [LEWI_A, LEWI_B]
%w(c).each_with_index { |v, i| require File.join("load_each_with_index", v) if i == 0 }
p LEWI_C
