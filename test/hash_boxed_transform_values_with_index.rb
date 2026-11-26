# transform_values.with_index and transform_keys.with_index on a boxed Hash
# number the block's calls
def order(keywords) = keywords.transform_values.with_index { |_keyword_class, index| index }

p order({"$id" => String, "type" => Integer})
p order([1].empty? ? nil : {"enum" => 1, "const" => "x"})

def from(keywords, start) = keywords.transform_values.with_index(start) { |v, i| "#{v}#{i}" }

p from({a: 1, b: 2}, 10)
p from({"a" => :x, "b" => 2.5}, 1)

def tagged(keywords) = keywords.transform_values.each_with_index { |v, i| [v, i] }

p tagged({a: 1, b: 2})
p tagged({"a" => nil, "b" => "s"})

def renamed(keywords) = keywords.transform_keys.with_index { |k, i| "#{k}#{i}" }

p renamed({a: 1, b: 2})
p renamed({"a" => nil, 3 => "s"})

def copied(keywords) = keywords.transform_values.each { |v| v.to_s }

p copied({a: 1, b: 2})
p copied({"a" => nil, 3 => "s"})

def missing(keywords)
  keywords.transform_values.with_index { |v, i| i }
rescue NoMethodError => e
  e.message
end

p missing({a: 1})
p missing({"a" => "b"})
p missing(nil)
