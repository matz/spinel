# frozen_string_literal: true
lo = +"aa"; hi = +"ad"; r = lo..hi; excl = lo...hi
p [:identity, r.begin.equal?(lo), r.first.equal?(lo), r.end.equal?(hi), r.last.equal?(hi),
   r.min.equal?(lo), r.max.equal?(hi), excl.min.equal?(lo), excl.max, excl.max.equal?(hi)]
p [:minmax, r.minmax[0].equal?(lo), r.minmax[1].equal?(hi), excl.minmax[0].equal?(lo)]
old = r.min; copy = r.dup
r.first.replace("ab"); hi.replace("ae")
p [:mutate, old, r.begin, r.end, copy.begin.equal?(lo), copy.end.equal?(hi), excl.max]
lo.replace("zz")
p [:empty, r.min, r.max, excl.min, excl.max]
a = +"ab"; b = +"ac"; assigned = a
q = Range.new(a, (a = +"different"; b), true)
p [:order, q.begin.equal?(assigned), q.end.equal?(b), q.exclude_end?, a]
p [:frozen, ("hello".."there").begin.equal?("hello"), ("hello".."there").begin.frozen?]
p [:open, (..hi).begin, (lo..).end, (lo..).min.equal?(lo), (..hi).max.equal?(hi)]
p [:exceptions, ((..hi).first rescue $!.class), ((lo..).last rescue $!.class)]

# A boxed String Range can yield members even when its minimum is nil.
names = [:"9", :"11", :a, true]
[("9"..."11"), ("2"..."02"), ("b".."a"), (""..""), (.."a"), ("a"..),
 (names[0]...names[1]), (names[2]...names[2]), true].each do |value|
  p value.to_h
rescue TypeError, NoMethodError => e
  puts e.message.lines.first.chomp
end
