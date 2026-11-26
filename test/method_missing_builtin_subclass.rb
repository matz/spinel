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
