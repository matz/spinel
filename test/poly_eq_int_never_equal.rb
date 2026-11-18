# An Integer compared with a boxed value that can never equal it (a String,
# Symbol, nil, a boolean, a builtin Array or Hash) answers false at once;
# values that can equal one, and a program's own ==, still answer as before.
class Always
  def ==(other) = true
end

class MyList < Array
  def ==(other) = other == 1
end

class MyMap < Hash
  def ==(other) = other == 1
end

def pick(i)
  [1, "1", :one, nil, true, false, [1], { 1 => 1 }, 1.0, 1r, Complex(1, 0), Always.new,
   MyList.new([1]), MyMap.new, 2][i]
end

labels = %w[int string symbol nil true false array hash float rational complex always mylist mymap two]
labels.each_with_index do |label, i|
  v = pick(i)
  matched = case v
            when 1 then "when 1"
            else "else"
            end
  # (an Integer on the left of a subclass's own == is a separate gap: left out)
  reverse = v.is_a?(MyList) || v.is_a?(MyMap) ? "-" : "#{1 == v} #{1 === v}"
  puts "#{label}: #{v == 1} #{reverse} #{matched}"
end

ops = [1, 2, [1, [2, 1]], 3, [3], 1]
count = Hash.new(0)
walk = lambda do |list|
  list.each do |op|
    case op
    when 1 then count[:one] += 1
    when 2 then count[:two] += 1
    when 3 then count[:three] += 1
    else walk.call(op)
    end
  end
end
walk.call(ops)
p count
