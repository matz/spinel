def invoke(&callback)
  values = {a: 1}
  callback.call(**values, &(values[:a] = 2; proc {}))
end
invoke { |**values, &block| p values }

def invoke_proc(&callback)
  values = {a: 1}
  callback.call(**values, &(values[:a] = 2; proc {}))
end
invoke_proc(&proc { |**values, &block| p values })
