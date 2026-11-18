# enum_for / to_enum of a program method that takes arguments: the method's
# required parameters are handed on, so `return enum_for(__method__, dir)
# unless block_given?` answers an Enumerator over the method's yields for
# that argument, as it does for a method that takes none.

class Lister
  def initialize(names) = @names = names

  # one argument, the blockless-branch idiom
  def entries(dir)
    return enum_for(__method__, dir).to_a unless block_given?
    @names.each { |n| yield "#{dir}/#{n}" }
  end

  # two arguments, named explicitly
  def pairs(prefix, sep)
    return to_enum(:pairs, prefix, sep) unless block_given?
    @names.each_with_index { |n, i| yield "#{prefix}#{sep}#{n}", i }
  end

  # an optional parameter: the helper takes none, as before
  def tagged(tag = "t")
    return enum_for(:tagged) unless block_given?
    @names.each { |n| yield "#{tag}:#{n}" }
  end

  # no argument, as before
  def each
    return enum_for(:each) unless block_given?
    @names.each { |n| yield n }
  end
end

l = Lister.new(%w[a b c])
p l.entries("d")
l.entries("e") { |path| puts path }
p l.pairs("x", "-").to_a
p l.pairs("y", ":").map { |s, i| "#{i}=#{s}" }
l.pairs("z", "+") { |s, i| puts "#{i} #{s}" }
p l.tagged.to_a
p l.each.to_a
p l.each.next
