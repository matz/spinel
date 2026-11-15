# A block called through a method's &param may itself receive a block.
class Callback
  def around(&callback)
    callback.call("entry") { "kept" }
  end

  def run
    around { |name, &execute| name + ":" + execute.call }
  end
end
puts Callback.new.run

def call_callback(&callback)
  callback.call("call") { |name| "kept:" + name }
end
def paren_callback(&callback)
  callback.("paren") { |name| "kept:" + name }
end
def index_callback(&callback)
  callback["index"] { |name| "kept:" + name }
end
def yield_callback(&callback)
  callback.yield("yield") { |name| "kept:" + name }
end
puts call_callback { |name, &execute| execute.call(name) }
puts paren_callback { |name, &execute| execute.call(name) }
puts index_callback { |name, &execute| execute.call(name) }
puts yield_callback { |name, &execute| execute.call(name) }

callback = proc { |name, &execute| execute.call(name) }
puts call_callback(&callback)
puts paren_callback(&callback)
puts index_callback(&callback)
puts yield_callback(&callback)
callback = ->(name, &execute) { execute.call(name) }
puts call_callback(&callback)
puts paren_callback(&callback)
puts index_callback(&callback)
puts yield_callback(&callback)

def nil_callback(&callback)
  callback.call(&nil)
end
puts nil_callback { |&execute| execute.nil? }

def nested_callback(&callback)
  callback.call { |&inner| "outer:" + inner.call }
end
puts nested_callback { |&execute| execute.call { "inner" } }

def argument_callback(&callback)
  callback.call("required") { "default" }
  callback.call("required", "optional", "extra", "last") { "supplied" }
end
argument_callback do |first, second = "omitted", *rest, &execute|
  puts [first, second, rest.join(":"), execute.call].join("|")
end

# A block with no &param drops the call's block without executing it.
puts call_callback { |name| name }

def save_callback(&callback)
  count = 0
  callback.call { count += 1; "stored:" + count.to_s }
end
def build_saved
  save_callback { |&execute| execute }
end
saved = build_saved
20.times { "allocation".dup }
puts saved.call
puts saved.call

def repeat_callback(&callback)
  callback.call { "first" }
  callback.call(&nil)
  callback.call { "last" }
end
repeat_callback { |&execute| puts(execute ? execute.call : "none") }
callback = proc { |&execute| puts(execute ? execute.call : "none") }
repeat_callback(&callback)

# Evaluate the arguments before the literal callback runs.
def ordered_callback(&callback)
  log = []
  callback.call((log << "arg"; "value")) { log << "called" }
  puts log.join(":")
end
ordered_callback { |value, optional = "default", &execute| puts value; execute.call }
ordered_callback(&proc { |value, optional = "default", &execute| puts value; execute.call })
