#include "helpers.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

int main(int argc, char* argv[])
{
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--network")
        {
            testNetworkFailures();
            TempDirectory temp;
            const auto archive = temp.path / "network-fixture.bak";
            writeBytes(archive, {'t', 'e', 's', 't'});
            testNetwork(temp.path, archive);
            return 0;
        }
        std::cout << "[1/4] SHA-256 and cipher known-answer vectors\n";
        testSha256();
        testCryptoVectors();
        testUnreadableSource();
        testMetadataFailure();
        testSyncFailure();
        testPipelineIo();
        testSourceFifoRace();
        testCancellation();
        testHardLinks();
        if (argc == 2 && std::string(argv[1]) == "--filesystem")
        {
            std::cout << "Filesystem regression tests passed.\n";
            return 0;
        }
        TempDirectory temp;
        fs::path source = temp.path / "testsrc";
        createFixture(source);
        std::vector<std::string> fixturePaths = listTree(source);

        std::cout << "[2/4] 18 complete-tree algorithm combinations\n";
        auto archives = testAllCombinations(temp.path, source, fixturePaths);
        std::cout << "[3/4] corruption, bounds, conflicts, and path races\n";
        testFailureModes(temp.path, source, archives);
        std::cout << "[4/4] account-isolated network roundtrip and session "
                     "recycling\n";
        testNetworkFailures();
        testNetwork(temp.path, archives.front().path);
        std::cout << "All backup-system tests passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "TEST FAILURE: " << error.what() << '\n';
        return 1;
    }
}
