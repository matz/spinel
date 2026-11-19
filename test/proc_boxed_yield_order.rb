class Yielder
  def yield(*values, **kw, &block)
    [values, kw, block&.call]
  end
end

def block_value
  puts "block"
  proc { "block result" }
end

def choose(values, index)
  puts "receiver"
  values[index]
end

def arguments
  puts "positionals"
  [1, 2]
end

def keywords
  puts "keywords"
  {marker: 3}
end

callback = -> (*values, **kw, &block) { [values, kw, block&.call] }
list = [callback, Yielder.new]
list.length.times do |index|
  p choose(list, index).yield(*arguments, **keywords, &block_value)
  p choose(list, index).yield
  p choose(list, index).yield(1, marker: 2) { "literal result" }
end
