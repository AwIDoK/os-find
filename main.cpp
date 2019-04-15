#include <iostream>
#include <vector>
#include <sstream>
#include <iterator>
#include <dirent.h>
#include <sys/types.h>
#include <stdexcept>
#include <cstring>
#include <cassert>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>


void print_help() {
    std::cout <<
              "Usage : ./os-find path [options]\n"
              "Recursively looks for files in given directory with given parameters and prints them to STDOUT\n"
              "options:\n"
              "\t-inum NUM\t\tlooks for files with given inode number\n"
              "\t-name NAME\t\tlooks for files with given name\n"
              "\t-size [=|-|+]SIZE\tlooks for files with given size in bytes\n"
              "\t\t =\t\t  size would be equal to SIZE\n"
              "\t\t +\t\t  size would be greater than SIZE\n"
              "\t\t -\t\t  size would be less than SIZE\n"
              "\t  If symbol isn't specified \'=\' will be used.\n"
              "\t-nlinks NUM\t\tlooks for file with given number of hard links\n"
              "\t-exec PATH\t\tresults will be provided to executable located at PATH as a single argument\n"
              "\t-help\t\tshows this message" << std::endl;
}

template<typename T>
class optional {
    T value;
    bool is_set = false;
  public:
    T get() const {
        assert(is_set == true);
        return value;
    }
    void set(T const& new_val) {
        is_set = true;
        value = new_val;
    }
    bool empty() const {
        return !is_set;
    }
};

uint64_t parse_num(const char* value, const char* option) {
    try {
        return std::stoull(value);
    } catch (std::logic_error const& e) {
        throw std::invalid_argument("Value of " + std::string(option) + " is invalid: " + value + ", " + e.what());
    }
}

class configuration {
    enum class size_type : char {EQUAL, GREATER, LESS};
    bool help = false;
    optional<ino_t> inode;
    char* name = nullptr;
    optional<std::pair<off_t, size_type>> size;
    optional<nlink_t> nlinks;
    char* executable = nullptr;

    std::pair<off_t, size_type> get_size(const char* value) {
        size_t pos = 0;
        size_type type;
        if (!isdigit(value[0])) {
            switch (value[0]) {
            case '+':
                type = size_type::GREATER;
                break;
            case '-':
                type = size_type::LESS;
                break;
            case '=':
                break;
            default:
                throw std::invalid_argument(std::string("Value of size is invalid: Unknown symbol ") + value[0]);
            }
            pos++;
        }
        if (std::strlen(value + pos) == 0 || value[pos] == '-') {
            throw std::invalid_argument(std::string("Value of size is invalid:") + value);
        }
        return std::make_pair(parse_num(value + pos, "-size"), type);
    }

  public:
    configuration(int argc, char **argv) {
        if (std::strcmp(argv[1], "-help") == 0) {
            help = true;
            return;
        }
        for (int i = 2; i < argc; i += 2) {
            const char* option = argv[i];
            if (std::strcmp(option, "-help") == 0) {
                help = true;
                break;
            }
            if (i + 1 == argc) {
                throw std::invalid_argument("Value of " + std::string(option) + " wasn't found");
            }
            char* value = argv[i + 1];
            if (std::strcmp(option, "-inum") == 0) {
                inode.set(parse_num(value, option));
            } else if (std::strcmp(option, "-name") == 0) {
                name = value;
            } else if (std::strcmp(option, "-size") == 0) {
                size.set(get_size(value));
            } else if (std::strcmp(option, "-nlinks") == 0) {
                nlinks.set(parse_num(value, option));
            } else if (std::strcmp(option, "-exec") == 0) {
                executable = value;
            } else {
                throw std::invalid_argument("Unknown option " + std::string(option));
            }
        }
    }

    bool is_help() const {
        return help;
    }

    bool matches(const dirent* file, const char* path) const {
        if (!inode.empty() && inode.get() != file->d_ino) {
            return false;
        }
        if (name && std::strcmp(file->d_name, name)) {
            return false;
        }
        struct stat st;
        if(stat(path, &st) == -1) {
            std::cerr << "Cannot check file " << path << ": " << strerror(errno) << std::endl;
            return false;
        }
        if (!size.empty()) {
            auto value = size.get().first;
            auto type = size.get().second;
            switch (type) {
            case size_type::EQUAL:
                if (st.st_size != value) {
                    return false;
                }
                break;
            case size_type::LESS:
                if (st.st_size >= value) {
                    return false;
                }
                break;
            case size_type::GREATER:
                if (st.st_size <= value) {
                    return false;
                }
            }
        }
        if (!nlinks.empty() && st.st_nlink != nlinks.get()) {
            return false;
        }
        return true;
    }

    char* get_executable() const {
        return executable;
    }

};

void show_error() {
    std::cerr << "The following error occurred: " << strerror(errno) << std::endl;
}

void process(configuration const& config, const char* name, char ** envp) {
    auto executable = config.get_executable();
    if (executable) {
        switch (auto pid = fork()) {
        case -1:
            std::cerr << "Cannot fork: " << strerror(errno) << std::endl;
            return;
        case 0: {
            std::vector<char*> args = {executable, const_cast<char*>(name), nullptr};
            if (execve(args[0], args.data(), envp) == -1) {
                show_error();
                exit(EXIT_FAILURE);
            }
            exit(EXIT_SUCCESS);
        }
        default:
            int status;
            if (waitpid(pid, &status, 0) == -1) {
                show_error();
            }
        }
    } else {
        std::cout << name << std::endl;
    }
}


class walker {
    configuration const& config;
    char** envp;
  public:
    walker(configuration const& config, char **envp) : config(config), envp(envp) { }
    void recursive_walk(const char* path) {
        auto dir = opendir(path);
        if (dir == nullptr) {
            std::cerr << "Cannot open " << path << ": " << strerror(errno) << std::endl;
            return;
        }
        while (auto entry = readdir(dir)) {
            auto full_path = std::string(path) + '/' + entry->d_name;
            if (entry->d_type == DT_DIR) {
                if (std::strcmp(entry->d_name, ".") == 0 || std::strcmp(entry->d_name, "..") == 0) {
                    continue;
                }
                recursive_walk(full_path.c_str());
            } else {
                if (config.matches(entry, full_path.c_str())) {
                    process(config, full_path.c_str(), envp);
                }
            }
        }
        if (closedir(dir) == -1) {
            std::cerr << "Cannot close " << path << ": " << strerror(errno) << std::endl;
            return;
        }

    }
};

int main(int argc, char **argv, char** envp) {
    if (argc < 2) {
        print_help();
        return EXIT_FAILURE;
    }
    try {
        configuration config(argc, argv);
        if (config.is_help()) {
            print_help();
            return EXIT_SUCCESS;
        }
        walker{config, envp}.recursive_walk(argv[1]);
    } catch (std::invalid_argument const& e) {
        std::cerr << e.what();
        return EXIT_FAILURE;
    }
}
