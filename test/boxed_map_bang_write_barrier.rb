# Boxed map! and collect! retain fresh block results across collections.
def maps(c)
  c.map! { |a| [a[1], a[0]] }
  c.collect! { |q| [q] }
  p c
end
def churn(n) = (r = []; n.times { |i| r << [i, i.to_s] }; r.size)
(0..40).each { |k| churn(k); maps([[1, 2], [3, 4]]) }
maps(nil) rescue p $!.class
