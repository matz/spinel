# A String local that is the shared handle (it is aliased and handed to a
# method that appends to it) assigned the value of a call no method answers
# (the respond_to?-guarded arm, raising NoMethodError when taken): the raise
# is kept and the local takes no String from it.
def app(s)
  s << "!"
  s.size
end

class Object
  def enc(opts)
    if respond_to?(:to_json)
      str = to_json(opts)
    else
      str = to_s
    end
    n = app(str)
    t = str
    puts t
    n
  end
end

p 12.enc(nil)
p :sym.enc(1)
