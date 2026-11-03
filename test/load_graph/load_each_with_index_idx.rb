# A computed require in `%w(...).each_with_index { |word, i| ... }` whose path
# reads the index names no file of the word alone: the literals of the path
# are word.rb for each word, where the program requires word/<i>.rb. The set
# of files such a require loads stays open (--load-model=graph), so it loads
# the file its path names and word.rb is not loaded. So does a path that reads
# any other local the loop does not bind to a value (here "0", but only the run
# time knows it).
module Layout
  LIBPATH = __dir__ + "/"
  def self.libpath(*a) = File.join(LIBPATH, *a)
end
if ARGV.size > 5
  require_relative "load_each_with_index_idx/x/0"
  require_relative "load_each_with_index_idx/y/1"
  require_relative "load_each_with_index_idx/z/0"
end
%w(x y).each_with_index { |w, i| require Layout.libpath(["load_each_with_index_idx", w, i.to_s]) }
p [LEWI_X, LEWI_Y]
sub = ARGV.size.to_s
%w(z).each { |w| require Layout.libpath(["load_each_with_index_idx", w, sub]) }
p LEWI_Z
p defined?(LEWI_X_FLAT), defined?(LEWI_Y_FLAT), defined?(LEWI_Z_FLAT)
