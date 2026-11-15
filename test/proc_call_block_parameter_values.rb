receiver = proc { |name, &execute| execute.call(name) }
puts(receiver.call("call") { |name| "proc:" + name })
puts(receiver.("paren") { |name| "proc:" + name })
puts(receiver["index"] { |name| "proc:" + name })
puts(receiver.yield("yield") { |name| "proc:" + name })

receiver = ->(name, &execute) { execute.call(name) }
puts(receiver.call("call") { |name| "lambda:" + name })
puts(receiver.("paren") { |name| "lambda:" + name })
puts(receiver["index"] { |name| "lambda:" + name })
puts(receiver.yield("yield") { |name| "lambda:" + name })
begin
  receiver.call("nil", &nil)
rescue NoMethodError => error
  puts error.class
end

# An inlined forwarder must also pass its block to a materialized proc.
def invoke_callback(&callback)
  callback.call("forwarded") { "kept" }
end
puts invoke_callback(&proc { |name, &execute| name + ":" + execute.call })
puts invoke_callback(&->(name, &execute) { name + ":" + execute.call })

def invoke_splat_callback(args, &callback)
  callback.call(*args) { "spread" }
end
puts invoke_splat_callback(["forwarded"], &receiver)

optional = ->(first, second = "omitted", *rest, &execute) do
  [first, second, rest.join(":"), execute.call].join("|")
end
puts(optional.call("required") { "default" })
puts(optional.call("required", "optional", "extra") { "supplied" })
begin
  optional.call { "too few" }
rescue ArgumentError => error
  puts error.class
end

class MethodCallback
  def run(name, &execute)
    execute ? name + ":" + execute.call : "none"
  end
end
method_proc = MethodCallback.new.method(:run).to_proc
puts(method_proc.call("method") { "kept" })
puts method_proc.call("method", &nil)

ignored = proc { "dropped" }
puts(ignored.call { raise "unexpected" })
ignored_lambda = -> { "dropped lambda" }
puts(ignored_lambda.call { raise "unexpected" })

count = 0
capture = proc { |&execute| execute.call; execute }
saved = capture.call { count += 1 }
20.times { "allocation".dup }
puts count
puts saved.call
puts count

# A nullable forwarded proc still raises for a missing receiver.
def choose_callback(flag)
  flag ? proc { |&execute| execute.call } : nil
end
def invoke_nullable_callback(&callback)
  callback.call { "nullable" }
end
def invoke_nullable_splat_callback(args, &callback)
  callback.call(*args) { "nullable spread" }
end
callback = choose_callback(false)
begin
  invoke_nullable_callback(&callback)
rescue NoMethodError => error
  puts error.class
end
begin
  invoke_nullable_splat_callback([], &callback)
rescue NoMethodError => error
  puts error.class
end
callback = choose_callback(true)
puts invoke_nullable_callback(&callback)
