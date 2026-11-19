# A call in a `begin` whose value is pushed, in an ensure, or with a nil-able
# receiver is in the window of its statement, as any other call is: it meets
# the version made before it runs and the one a Proc makes after.
class H
  def baz = "baz"
end
def chk(tag)
  r = []
  r << begin; H.new.baz; rescue NameError => e; e.class.name; end
  begin; r << 0; ensure; r << begin; H.new.baz; rescue NameError => e; e.class.name; end; end
  r.push(begin; H.new.baz; rescue NameError => e; e.class.name; end)
  r << (H.new.baz rescue "err")
  h = tag == :none ? nil : H.new
  r << begin; h.baz; rescue NoMethodError => e; e.class.name; end
  puts "#{tag} #{r.inspect}"
end
def l3 = H.new.baz
def l2 = l3
def levels(tag)
  r = []
  r << begin; l2; rescue NameError => e; e.class.name; end
  puts "#{tag} #{r.inspect}"
end
pr = proc do
  class H
    def baz = "new"
  end
  nil
end
chk(:before)
levels(:before)
chk(:none)
pr.call
chk(:after)
levels(:after)
at_exit { chk(:exit); levels(:exit) }
at_exit do
  class H
    def baz = "exit"
  end
  nil
end
