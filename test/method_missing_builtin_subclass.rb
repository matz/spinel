# A subclass of a builtin that defines its own method_missing keeps the
# builtin's methods: `length`, `replace` (String.new's), `<<` and the rest
# are answered above the chain, and only a name no ancestor has reaches
# method_missing.
class Inquirer < String
  def method_missing(name, *args)
    return self == name.to_s.chomp("?") if name.end_with?("?")
    raise NotImplementedError, "mm #{name}"
  end
end
env = Inquirer.new("development")
p env.length
p env.to_s
p "#{env}.rb"
p(env == "development")

class Bag < Array
  def method_missing(name, *args)
    return size if name == :count_them
    raise NotImplementedError, "mm #{name}"
  end
end
b = Bag.new
b << 1
b.push(2, 3)
p b.length
p b.count_them
p b.first

# An undef_method on the chain takes the builtin's answer away: the name
# reaches method_missing, as it does in CRuby. (A String subclass's receiver
# is typed String and is not split here; its undef is a separate matter.)
class Muted < Array
  undef_method :length
  def method_missing(name, *args)
    return :muted if name == :length
    super
  end
end
m = Muted.new
m << 1
p m.length
p m.size
