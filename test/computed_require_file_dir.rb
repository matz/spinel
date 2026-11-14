# Requires built on __FILE__, the pre-require_relative gem idiom, load the
# file beside the requiring one.
require File.expand_path('computed_require_file_dir/one', File.dirname(__FILE__))
require File.join(File.expand_path(File.dirname(__FILE__)), 'computed_require_file_dir', 'two')
require File.expand_path(File.dirname(__FILE__)) + '/computed_require_file_dir/three'
require File.expand_path('../computed_require_file_dir/four', __FILE__)
require File.join(__dir__, 'computed_require_file_dir/five.rb')
require File.expand_path(File.join(File.dirname(__FILE__), 'computed_require_file_dir', 'sub', 'six'))
p [CRFD.one, CRFD.two, CRFD.three, CRFD.four, CRFD.five, CRFD.six]
raise "line numbers moved" unless __LINE__ == 10
