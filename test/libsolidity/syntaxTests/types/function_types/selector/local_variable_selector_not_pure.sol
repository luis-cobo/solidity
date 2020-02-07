contract C {
    function f() public pure returns (bytes4) {
        function() external g;
        g.selector;
    }
}
