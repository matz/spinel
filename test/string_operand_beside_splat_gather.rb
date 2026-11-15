# A String slot's read beside a splat and an operand that runs code: the
# operands that run code are bound ahead in order (the String group), and a
# splat's temp holds the Array it gathers, whatever type the node says of
# its elements. Typed as the elements', the temp was an sp_int or a const
# char * and the program did not compile under --int-overflow=promote.
class Todo
  def add(title) = "add #{title.inspect}"
end

def run(target, name, args)
  target.send(name, *args)
end

p run(Todo.new, "add", ["x"])

k = +"STRING_OPERAND_BESIDE_SPLAT_K"
keys = ["STRING_OPERAND_BESIDE_SPLAT_A", "STRING_OPERAND_BESIDE_SPLAT_B"]
ENV[keys[0]] = "1"
ENV[keys[1]] = "2"
p ENV.values_at(*keys, (ENV[k] = "z"; k))
p ENV[k]
ENV.delete(k)
keys.each { |e| ENV.delete(e) }
