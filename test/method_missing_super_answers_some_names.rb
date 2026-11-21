# A method_missing that answers some names and hands the rest to `super`
# (BasicObject#method_missing, which raises the NoMethodError naming the
# call) answers the calls it takes, and the others raise that error with
# the name, the arguments and the receiver

class Finder
  def method_missing(name, *args)
    if name.to_s.start_with?("find_by_")
      "#{name.to_s.delete_prefix("find_by_")}=#{args.first.inspect}"
    else
      super
    end
  end

  def respond_to_missing?(name, include_private = false) = name.to_s.start_with?("find_by_") || super
end

class Special < Finder; end

class Short
  def method_missing(n, *a) = n == :line ? 42 : super
end

class Explicit
  def method_missing(n, x = 0)
    return x * 2 if n == :twice
    super(n, x, :extra)
  end
end

class InBlock
  def method_missing(n, *a)
    [1].each { |v| return v + 10 if n == :eleven; super }
  end
end

class Outer
  def method_missing(n, *a) = n == :outer ? :from_outer : super
end

class Inner < Outer
  def method_missing(n, *a) = n == :inner ? :from_inner : super
end

class Blank < BasicObject
  def method_missing(n, *a) = n == :blank ? :answered : super
end

class Table < Hash
  def method_missing(n, *a) = n == :table ? :answered : super
end

class Row < Array
  def method_missing(n, *a) = n == :row ? :answered : super
end

class WideRow < Row; end

class Asks
  def method_missing(n, *a) = n == :above ? defined?(super).inspect : super
end

class Checked
  def method_missing(n, *a) = n == :ok ? 1 : super(n, 1 / 0)
end

def show
  yield
rescue NoMethodError => e
  p [e.class, e.message, e.name, e.args, e.receiver.class]
end

p Finder.new.find_by_name("x")
p Special.new.find_by_id(3)
p Special.new.respond_to?(:find_by_q)
show { Finder.new.zork(1, :a) }
show { Special.new.zork }
show { [Finder.new, Special.new][1].other(2) }
p Short.new.line
show { Short.new.column }
p Explicit.new.twice(4)
show { Explicit.new.thrice(5) }
p InBlock.new.eleven
show { InBlock.new.twelve(1) }
show { Finder.new.method_missing(:direct, 7) }
p Inner.new.inner, Inner.new.outer
show { Inner.new.neither(3) }
p Blank.new.blank
begin; Blank.new.other(1); rescue NoMethodError => e; p [e.message, e.name, e.args]; end
p Table.new.table, WideRow.new.row
show { Table.new.missing(2) }
show { WideRow.new.missing }
p Asks.new.above
begin; Finder.new.method_missing("named"); rescue ArgumentError => e; p e.message; end
begin; Checked.new.method_missing("first"); rescue ZeroDivisionError, ArgumentError => e; p e.class; end
