# In an instance_eval block, a call on the result of a receiverless call
# that method_missing answers reaches the method of the class the hook
# returns, when the hook returns values of more than one class.
class Column
  def lt(other) = [:lt, other]
  def <(other) = [:<, other]
end

class Names
  def method_missing(name, *args)
    return Column.new if name == :created_at
    return 30 if name == :before
    raise NoMethodError, "undefined method '#{name}'"
  end
end

names = Names.new
p names.instance_eval { self.created_at.lt(30) }
p names.instance_eval { created_at.lt(30) }
p names.instance_eval { c = created_at; c.lt(before) }
p names.instance_eval { created_at < before }
p names.instance_eval { before + 1 }

def where(&block) = Names.new.instance_eval(&block)
p where { created_at.lt(30) }
p where { created_at < before }
