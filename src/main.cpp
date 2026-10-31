// #include <pqxx/pqxx>
// #include <cstdlib>
// #include <iostream>

// using namespace std;    

// int main(){
//     const char* url= getenv("DATABASE_URL");
//     if(!url){
//         cerr<<"ERROR: Fetching Databse URL\n";
//         return 1;
//     }

//     try{
//         pqxx::connection conn(url);
//         pqxx::work tx(conn);

//         auto r = tx.exec("SELECT id, name FROM sites WHERE id = $1", pqxx::params{1});
//         for(auto x:r)
//             cout<<x[0].as<int>()<<"  "<<x[1].as<string>()<<'\n';
//         tx.commit();
//     }catch(const pqxx::sql_error &e){
//         cerr<<"ERROR Occured: DB/SQL error: "<<e.what()<<'\n';
//     }catch(const exception &e){
//         cerr<<"ERROR Occurec: "<<e.what()<<'\n';
//     }
// }
#include <cstdlib>
#include <iostream>

#include "auth/password.hpp"
#include "db/Pool.hpp"
#include "handlers/admin.hpp"
#include "handlers/authorization.hpp"
#include "handlers/stock.hpp"
#include "http/App.hpp"
#include "handlers/reports.hpp"
#include "handlers/pallet.hpp"

int main(int argc, char** argv) {
    pw::init();

    // helper: ./build/inventory hashpw 'MyPassword'   -> prints a hash to store in users.password_hash
    if (argc == 3 && std::string(argv[1]) == "hashpw") {
        std::cout << pw::hash(argv[2]) << "\n";
        return 0;
    }

    const char* url = std::getenv("DATABASE_URL");
    if (!url) { std::cerr << "DATABASE_URL not set\n"; return 1; }
    try { jwtSecret(); }
    catch (const std::exception& e) { std::cerr << e.what() << "\n"; return 1; }
    const char* p = std::getenv("PORT");
    int port = p ? std::atoi(p) : 8080;

    Pool pool(url, 4);
    App app;

    app.get("/health", [](Request&, Response& res) { res.json(200, {{"ok", true}}); });

    registerAuthRoutes(app, pool);
    registerReportRoutes(app, pool);
    registerStockRoutes(app, pool);
    registerAdminRoutes(app, pool);
    registerPalletRoutes(app, pool);
    app.listen(port);
}