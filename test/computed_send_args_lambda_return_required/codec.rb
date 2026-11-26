Pairs = lambda { |value| return value unless value.is_a?(Array); value.each_slice(2).to_a }
