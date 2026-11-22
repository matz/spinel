# (:a..:e): a literal Range of Symbols is a Range, as one built at run time
# is: it enumerates by name succession (to_a/map/each/include?, exclusive
# ends, multi-char names), and it answers its ends, prints and compares as
# a Range.
p (:a..:e).to_a
p((:a..:c).map(&:to_s))
p (:a...:d).to_a
acc = []
(:x..:z).each { |s| acc << s }
p acc
p (:a..:e).include?(:c)
p (:aa..:ac).to_a

r = :a..:c
p r, r.class
p r.begin, r.end, r.first, r.last
p r.exclude_end?, (:a...:c).exclude_end?
p r.min, r.max, r.minmax
p r.size, r.count
p r.first(2), r.last(2)
p r.cover?(:bb), r.include?(:bb), r === :b
p r == (:a..:c), r == (:a...:c), r.eql?(:a..:c), r.hash == (:a..:c).hash
p (:b..:a).to_a, (:a..:a).to_a

# held in a container, and as a Hash key
x = [:a..:c, 1][0]
p x, x.begin, x.end
h = { (:a..:c) => 1 }
p h[:a..:c]

case :b
when :a..:c then puts "in"
else puts "out"
end
for s in :x..:z do p s end
