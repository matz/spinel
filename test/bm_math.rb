# Math module methods. All should return Float.

# Trig (in radians)
puts (Math.sin(0.0) * 1000).to_i        # 0
puts (Math.cos(0.0) * 1000).to_i        # 1000
puts (Math.tan(0.0) * 1000).to_i        # 0
puts (Math.sin(0.5) * 1000).to_i        # 479
puts (Math.cos(0.5) * 1000).to_i        # 877
puts (Math.tan(0.5) * 1000).to_i        # 546

# Inverse trig
puts (Math.atan(1.0) * 1000).to_i       # 785
puts (Math.atan2(1.0, 1.0) * 1000).to_i # 785
puts (Math.asin(1.0) * 1000).to_i       # 1570
puts (Math.acos(0.0) * 1000).to_i       # 1570

# Powers / logs
puts (Math.sqrt(2.0) * 1000).to_i       # 1414
puts (Math.log(1.0) * 1000).to_i        # 0
puts (Math.log2(8.0) * 1000).to_i       # 3000
puts (Math.log10(100.0) * 1000).to_i    # 2000
puts (Math.exp(0.0) * 1000).to_i        # 1000

# Float-typed result (would print "1" / "0" if inferred as int).
# Use the *1000+to_i idiom to dodge precision-formatting differences
# between Spinel's float-puts and CRuby's.
puts (Math.log2(3.0) * 1000).to_i       # 1584
puts (Math.log10(3.0) * 1000).to_i      # 477

# Hypot
puts (Math.hypot(3.0, 4.0) * 1000).to_i # 5000

# PI
puts (Math::PI * 1000).to_i             # 3141
