class TestLuaPredicate < CLIPPTest::TestCase
  include CLIPPTest

  def make_config(file, extras = {})
    return {
      :modules => ['lua', 'pcre', 'htp'],
      :predicate => true,
      :config => "LuaInclude \"#{file}\"\n",
      :default_site_config => "RuleEnable all",
    }.merge(extras)
  end

  def lua_path
    File.expand_path(File.join(BUILDDIR, "lua_test_" + rand(10000).to_s + '.lua'))
  end

  def test_basic
    lua = <<-EOS
      Action("basic1", "1"):
        phase([[REQUEST_HEADER]]):
        action([[clipp_announce:basic1]]):
        predicate(P.Operator('rx', 'GET', P.Var('REQUEST_METHOD')))
    EOS
    lua_file = lua_path()
    File.open(lua_file, 'w') {|fp| fp.print lua}

    clipp(make_config(lua_file,
      :input => "echo:\"GET /foo\""
    ))
    assert_no_issues
    assert_log_match /CLIPP ANNOUNCE: basic1/
  end

  def test_template
    lua = <<-EOS
      local getField = PUtil.Define('tc_lua_predicate_getField', {'name'},
        P.Var(P.Ref('name'))
      )
      Action("basic1", "1"):
        phase([[REQUEST_HEADER]]):
        action([[clipp_announce:basic1]]):
        predicate(P.Operator('rx', 'GET', getField('REQUEST_METHOD')))
    EOS
    lua_file = lua_path()
    File.open(lua_file, 'w') {|fp| fp.print lua}

    clipp(make_config(lua_file,
      :input => "echo:\"GET /foo\""
    ))
    assert_no_issues
    assert_log_match /CLIPP ANNOUNCE: basic1/
  end

  def test_string_replace_rx
    lua = <<-EOS
      Action("basic1", "1"):
        phase([[REQUEST_HEADER]]):
        action([[clipp_announce:srr1]]):
        predicate(
          P.P(P.StringReplaceRx('a', 'b', 'bar'))
        )
    EOS
    lua_file = lua_path()
    File.open(lua_file, 'w') {|fp| fp.print lua}

    clipp(make_config(lua_file,
      :input => "echo:\"GET /foo\""
    ))
    assert_no_issues
    assert_log_match /CLIPP ANNOUNCE: srr1/
    assert_log_match "'bbr'"
  end

  def test_foperator
    lua = <<-EOS
      Action("basic1", "1"):
        phase([[REQUEST_HEADER]]):
        action([[clipp_announce:foperator]]):
        predicate(P.P(P.FOperator('rx', 'a', P.Cat('a', 'ab', 'cb'))))
    EOS
    lua_file = lua_path()
    File.open(lua_file, 'w') {|fp| fp.print lua}

    clipp(make_config(lua_file,
      :input => "echo:\"GET /foo\""
    ))
    assert_no_issues
    assert_log_match /CLIPP ANNOUNCE: foperator/
    assert_log_match "['a' 'ab']"
  end
end
