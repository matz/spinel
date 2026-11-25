TopLambda = lambda { |xs|
  xs.each { |x| return x if x.even? }
  :none
}

KernelLambda = Kernel.lambda { [1].map { return :kernel } }
RootKernelLambda = ::Kernel.lambda { return :root_kernel }
TapLambda = lambda { [1].tap { return :tap } }
HashLambda = lambda { {x: 1}.each { return :hash } }
RangeLambda = lambda { (1..2).each { return :range } }

class LambdaClass
  Answer = lambda { return 42 }
end
