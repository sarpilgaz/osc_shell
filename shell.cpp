/**
	* Shell framework
	* course Operating Systems
	* Radboud University
	* v22.09.05

	Student names:
	- Sarp Ilgaz
	- Alex Kush
*/

/**
 * Hint: in most IDEs (Visual Studio Code, Qt Creator, neovim) you can:
 * - Control-click on a function name to go to the definition
 * - Ctrl-space to auto complete functions and variables
 */

// function/class definitions you are going to use
#include <iostream>
#include <unistd.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <sys/param.h>
#include <signal.h>
#include <string.h>
#include <assert.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <vector>
#include <list>
#include <optional>

// although it is good habit, you don't have to type 'std' before many objects by including this line
using namespace std;

extern char *__progname; //to have better error reporting when a program fails.

struct Command {
  vector<string> parts = {};
};

struct Expression {
  vector<Command> commands;
  string inputFromFile;
  string outputToFile;
  bool background = false;
  bool correct_file_IO_syntax = true; //used to detect malformed I/O operators. See specifications
};

// Parses a string to form a vector of arguments. The separator is a space char (' ').
vector<string> split_string(const string& str, char delimiter = ' ') {
  vector<string> retval;
  for (size_t pos = 0; pos < str.length(); ) {
    // look for the next space
    size_t found = str.find(delimiter, pos);
    // if no space was found, this is the last word
    if (found == string::npos) {
      retval.push_back(str.substr(pos));
      break;
    }
    // filter out consequetive spaces
    if (found != pos)
      retval.push_back(str.substr(pos, found-pos));
    pos = found+1;
  }
  return retval;
}

// wrapper around the C execvp so it can be called with C++ strings (easier to work with)
// always start with the command itself
// DO NOT CHANGE THIS FUNCTION UNDER ANY CIRCUMSTANCE
int execvp(const vector<string>& args) {
  // build argument list
  const char** c_args = new const char*[args.size()+1];
  for (size_t i = 0; i < args.size(); ++i) {
    c_args[i] = args[i].c_str();
  }
  c_args[args.size()] = nullptr;
  // replace current process with new process as specified
  int rc = ::execvp(c_args[0], const_cast<char**>(c_args));
  // if we got this far, there must be an error
  int error = errno;
  // in case of failure, clean up memory (this won't overwrite errno normally, but let's be sure)
  delete[] c_args;
  errno = error;
  return rc;
}

// Executes a command with arguments. In case of failure, returns error code.
int execute_command(const Command& cmd) {
  auto& parts = cmd.parts;
  if (parts.size() == 0)
    return EINVAL;

  // execute external commands
  int retval = execvp(parts);
  return retval ? errno : 0;
}

void display_prompt() {
  char buffer[512];
  char* dir = getcwd(buffer, sizeof(buffer));
  if (dir) {
    cout << "\e[32m" << dir << "\e[39m"; // the strings starting with '\e' are escape codes, that the terminal application interpets in this case as "set color to green"/"set color to default"
  }
  cout << "$ ";
  flush(cout);
}

string request_command_line(bool showPrompt) {
  if (showPrompt) {
    display_prompt();
  }
  string retval;
  getline(cin, retval);
  return retval;
}

// note: For such a simple shell, there is little need for a full-blown parser (as in an LL or LR capable parser).
// Here, the user input can be parsed using the following approach.
// First, divide the input into the distinct commands (as they can be chained, separated by `|`).
// Next, these commands are parsed separately. The first command is checked for the `<` operator, and the last command for the `>` operator.
Expression parse_command_line(string commandLine) {
  Expression expression;
  vector<string> commands = split_string(commandLine, '|');
  for (size_t i = 0; i < commands.size(); ++i) {
    string& line = commands[i];
    vector<string> args = split_string(line, ' ');
    if (i == commands.size() - 1 && args.size() > 1 && args[args.size()-1] == "&") {
      expression.background = true;
      args.resize(args.size()-1);
    }
    if (i == commands.size() - 1 && args.size() > 2 && args[args.size()-2] == ">") {
      expression.outputToFile = args[args.size()-1];
      args.resize(args.size()-2);
    }
    else if (i != commands.size() - 1 && args.size() > 2 && args[args.size()- 2] == ">") {
      expression.correct_file_IO_syntax = false;
    }
    if (i == 0 && args.size() > 2 && args[args.size()-2] == "<") {
      expression.inputFromFile = args[args.size()-1];
      args.resize(args.size()-2);
    }
    else if (i != 0 && args.size() > 2 && args[args.size()- 2] == "<") {
      expression.correct_file_IO_syntax = false;
    }
    expression.commands.push_back({args});
  }
  return expression;
}

int execute_expression(Expression& expression) {
  // Check for empty expression
  if (expression.commands.size() == 0)
    return EINVAL;

  //check for incorrect file I/O syntax
  if (!expression.correct_file_IO_syntax) {
    fprintf(stderr, "Incorrect usage of file I/O operators detected\n");
    fprintf(stderr, "operator '>' can only be used in the last command of a pipe \n");
    fprintf(stderr, "operator '<' can only be used in the first command of a pipe \n");
    return EXIT_FAILURE;
  }

  // Handle intern commands (like 'cd' and 'exit')
  if (expression.commands[0].parts[0] == string("exit")) {
    exit(EXIT_SUCCESS);
  }

  if (expression.commands[0].parts[0] == string("cd")) {
    if (expression.commands[0].parts.size() != 2) {
        fprintf(stderr, "Usage: cd <directory>\n");
        return EXIT_FAILURE;
    }
    if (chdir(expression.commands[0].parts[1].c_str()) == -1) {
      perror("cd");
      return EXIT_FAILURE;
    } else return EXIT_SUCCESS;
  }
  
  // External commands, executed with fork():
  // Loop over all commands, and connect the output and input of the forked processes

  int numCommands = expression.commands.size();
  int pipefds[2 * (numCommands - 1)];

  // Create pipes for each command except the last one
  for (int i = 0; i < numCommands - 1; i++) {
    if (pipe(pipefds + i * 2) == -1) {
      perror("pipe");
      return errno;
    }
  }
  for (int i = 0; i < numCommands; i++) {
    pid_t pid = fork();
    if (pid == -1) {
      perror("fork");
      return errno;
    }
    if (pid == 0) { //child

      if (!expression.inputFromFile.empty()) {
        int inputfd = open(expression.inputFromFile.c_str(), O_RDONLY);
        if (inputfd < 0) {
          perror(expression.inputFromFile.c_str());
          _exit(EXIT_FAILURE);
        }  
        dup2(inputfd, STDIN_FILENO);
        close(inputfd);
      }

      if (!expression.outputToFile.empty()) {
        int outputfd = open(expression.outputToFile.c_str(), O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
        if (outputfd < 0) {
          perror(expression.outputToFile.c_str());
          _exit(EXIT_FAILURE);
        }  
      dup2(outputfd, STDOUT_FILENO);
      close(outputfd);             
      }

      if (i > 0) { //get input from previous command, if not the first
        dup2(pipefds[(i - 1) *2], STDIN_FILENO);
      }

      if (i < numCommands - 1) { //send output to next command, if not the last
        dup2(pipefds[i * 2 + 1], STDOUT_FILENO);
      }
      //clean up pipes
      for (int j = 0; j < 2 * (numCommands - 1); j++) {
        close(pipefds[j]);
      }

      int ret = execute_command(expression.commands[i]);
      if (ret != 0) {
        perror(__progname);
        _exit(EXIT_FAILURE);        
      }
      _exit(EXIT_SUCCESS);
    }
  }
    //pipes close in parent
    for (int i = 0; i < 2 * (numCommands - 1); i++) {
        close(pipefds[i]);
    }

    // wait in parent for children, expect for background processes.
    for (int i = 0; i < numCommands; i++) {
      if (!expression.background)  { wait(nullptr); }
    }

  return EXIT_SUCCESS;
}

// framework for executing "date | tail -c 5" using raw commands
// two processes are created, and connected to each other
int step1(bool showPrompt) {
  // create communication channel shared between the two processes
  int pipefd[2];

  if (pipe(pipefd) == -1) {
    cout << "no";
    return -1;
  }

  pid_t child1 = fork();
  if (child1 == 0) {
    // redirect standard output (STDOUT_FILENO) to the input of the shared communication channel
    // free non used resources (why?)
    dup2(pipefd[1], STDOUT_FILENO);

    close(pipefd[0]);
    close(pipefd[1]);

    Command cmd = {{string("date")}};
    execute_command(cmd);
    // display nice warning that the executable could not be found
    abort(); // if the executable is not found, we should abort. (why?)
  }

  pid_t child2 = fork();
  if (child2 == 0) {
    // redirect the output of the shared communication channel to the standard input (STDIN_FILENO).
    // free non used resources (why?)
    dup2(pipefd[0], STDIN_FILENO);

    close(pipefd[0]);
    close(pipefd[1]);

    Command cmd = {{string("tail"), string("-c"), string("5")}};
    execute_command(cmd);
    abort(); // if the executable is not found, we should abort. (why?)
  }

  close(pipefd[0]);  // Close read end
  close(pipefd[1]);  // Close write end

  // wait on child processes to finish (why both?)
  waitpid(child1, nullptr, 0);
  waitpid(child2, nullptr, 0);
  return 0;
}

int shell(bool showPrompt) {
  //* <- remove one '/' in front of the other '/' to switch from the normal code to step1 code
  while (cin.good()) {
    string commandLine = request_command_line(showPrompt);
    Expression expression = parse_command_line(commandLine);
    int rc = execute_expression(expression);
    if (rc != 0)
      cerr << strerror(rc) << endl;
  }
  return 0;
  /*/
  return step1(showPrompt);
  //*/
}