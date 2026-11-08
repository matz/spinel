# A proc or lambda that hands its own `&b` to a builtin iterator: the
# builtin calls the block the proc was given, and with none it answers its
# blockless form. The two lambdas are the form TSort's module functions take.
g = {1 => [2, 3], 2 => [3], 3 => []}
each_node = lambda { |&b| g.each_key(&b) }
each_child = lambda { |n, &b| g[n].each(&b) }
each_node.call { |n| p n }
each_child.call(1) { |c| p c }

pr = proc { |&b| [1, 2].map(&b) }
p pr.call { |x| x * 10 }
arrow = ->(&b) { (1..3).select(&b) }
p arrow.call(&:odd?)
p arrow.call { |x| x > 1 }

# no block: the builtin's Enumerator, as CRuby's `each(&nil)` answers
p each_node.call.class
p each_node.call.to_a
p pr.call.class

# an outer local of the same name is not the lambda's
b = 5
each_node.call { |n| print n }
puts
p b
