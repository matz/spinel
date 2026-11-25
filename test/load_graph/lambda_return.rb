class LambdaSettings
  [:color].each { |k| define_method("#{k}_label") { k.to_s } }
end

require_relative "lambda_return_lib"

p TopLambda.call([1, 2, 3])
p TopLambda.call([1, 3])
p KernelLambda.call
p RootKernelLambda.call
p TapLambda.call
p HashLambda.call
p RangeLambda.call
p LambdaClass::Answer.call
def load_later
  require_relative "lambda_return_later"
end
load_later
p LaterLambda.call("later")
p LaterNestedLambda.call([1, 2])
p LambdaSettings.new.color_label
