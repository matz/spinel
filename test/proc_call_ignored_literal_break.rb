# A block unused by its receiver keeps its existing call-site control flow.
def invoke_unused(&callback)
  callback.call { break "unused" }
end
puts invoke_unused { |&unused| "kept-inline" }
puts invoke_unused(&proc { |&unused| "kept-proc" })
puts invoke_unused(&->(&unused) { "kept-lambda" })
puts invoke_unused { "kept-without-parameter" }
