add_executable(TokenBmpDecoderTest
  TokenBmpDecoderTest.cpp
  ${REPO_ROOT}/src/network/TokenBmpDecoder.cpp
)

target_link_libraries(TokenBmpDecoderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TokenBmpDecoderTest)
