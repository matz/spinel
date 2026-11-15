# yield and block_given? in a receiving block use its enclosing method's
# block, not the distinct block passed to the Proc's call.
def lexical_callback
  proc do |&execute|
    puts block_given?
    puts yield("lexical")
    puts execute.call
  end
end
receiver = lexical_callback { |name| "outer:" + name }
receiver.call { "inner" }

def inline_callback(&callback)
  callback.call { "inner" }
end
def lexical_inline
  inline_callback do |&execute|
    puts block_given?
    puts yield("inline")
    puts execute.call
  end
end
lexical_inline { |name| "outer:" + name }

def lexical_without_outer
  inline_callback do |&execute|
    puts block_given?
    begin
      yield
    rescue LocalJumpError => error
      puts error.class
    end
    puts execute.call
  end
end
lexical_without_outer

def ignores_call_block
  inline_callback do
    puts block_given?
    puts yield("ignored")
  end
end
ignores_call_block { |name| "outer:" + name }
